// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace victus::animation {
namespace {

struct ParamTypeName {
    std::string_view name;
    ParamType type;
};
constexpr std::array kParamTypes{ParamTypeName{"float", ParamType::Float}, ParamTypeName{"int", ParamType::Int},
                                 ParamTypeName{"bool", ParamType::Bool}, ParamTypeName{"trigger", ParamType::Trigger}};

struct OpName {
    std::string_view name;
    ConditionOp op;
};
constexpr std::array kOps{OpName{"true", ConditionOp::True},      OpName{"false", ConditionOp::False},
                          OpName{"trigger", ConditionOp::Trigger}, OpName{">", ConditionOp::Greater},
                          OpName{">=", ConditionOp::GreaterEqual}, OpName{"<", ConditionOp::Less},
                          OpName{"<=", ConditionOp::LessEqual},   OpName{"==", ConditionOp::Equal},
                          OpName{"!=", ConditionOp::NotEqual}};

Error fail(std::string_view where, std::string_view what) {
    return make_error(ErrorCode::ParseError, std::format("animation controller: {}: {}", where, what));
}

bool finite(float v) noexcept { return std::isfinite(v); }

// Reads an optional finite number; `out` keeps its default when the key is absent.
Status read_number(const json::Value& obj, std::string_view key, std::string_view where, float& out) {
    const json::Value* v = obj.find(key);
    if (!v) return {};
    const float f = v->as_float(NAN);
    if (!v->is_number() || !finite(f)) return fail(where, std::format("\"{}\" must be a finite number", key));
    out = f;
    return {};
}

Status read_string(const json::Value& obj, std::string_view key, std::string_view where, std::string& out,
                   bool required) {
    const json::Value* v = obj.find(key);
    if (!v) {
        if (required) return fail(where, std::format("missing \"{}\"", key));
        return {};
    }
    if (!v->is_string()) return fail(where, std::format("\"{}\" must be a string", key));
    out = v->as_string();
    if (required && out.empty()) return fail(where, std::format("\"{}\" must not be empty", key));
    return {};
}

Status read_array(const json::Value& obj, std::string_view key, std::string_view where, std::size_t limit,
                  const json::Value*& out) {
    out = obj.find(key);
    if (!out) return {};
    if (!out->is_array()) return fail(where, std::format("\"{}\" must be an array", key));
    if (out->size() > limit) return make_error(ErrorCode::LimitExceeded,
                                               std::format("animation controller: {}: more than {} \"{}\"", where,
                                                           limit, key));
    return {};
}

bool is_numeric(ParamType t) noexcept { return t == ParamType::Float || t == ParamType::Int; }

// Re-anchors a nested layer error: "animation controller: states[0]: x" -> "...: layers[1].states[0]: x".
Error nest_error(Error e, std::string_view where) {
    constexpr std::string_view kPrefix = "animation controller: ";
    if (e.message.starts_with(kPrefix))
        e.message = std::string(kPrefix) + std::string(where) + "." + e.message.substr(kPrefix.size());
    return e;
}

Status read_names(const json::Value& obj, std::string_view key, std::string_view where, std::vector<std::string>& out) {
    const json::Value* arr = nullptr;
    if (Status st = read_array(obj, key, where, Controller::kMaxMaskNodes, arr); !st) return st;
    for (std::size_t i = 0; arr && i < arr->size(); ++i) {
        const json::Value& v = (*arr)[i];
        if (!v.is_string() || v.as_string().empty() || v.as_string().size() > 256)
            return fail(where, std::format("\"{}\"[{}] must be a non-empty node name", key, i));
        out.push_back(v.as_string());
    }
    return {};
}

Result<std::shared_ptr<const Controller>> parse_machine(const json::Value& doc, std::string asset_name,
                                                        const std::vector<ControllerParam>* inherited);

} // namespace

std::string_view param_type_name(ParamType t) noexcept {
    for (const auto& p : kParamTypes)
        if (p.type == t) return p.name;
    return "float";
}

std::string_view condition_op_name(ConditionOp op) noexcept {
    for (const auto& o : kOps)
        if (o.op == op) return o.name;
    return "true";
}

std::int32_t Controller::find_state(std::string_view n) const noexcept {
    for (std::size_t i = 0; i < states.size(); ++i)
        if (states[i].name == n) return static_cast<std::int32_t>(i);
    return -1;
}

std::int32_t Controller::find_parameter(std::string_view n) const noexcept {
    for (std::size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].name == n) return static_cast<std::int32_t>(i);
    return -1;
}

std::int32_t Controller::find_layer(std::string_view n) const noexcept {
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layers[i].name == n) return static_cast<std::int32_t>(i);
    return -1;
}

std::string_view layer_blend_name(LayerBlend b) noexcept { return b == LayerBlend::Additive ? "additive" : "override"; }

std::vector<std::string> Controller::clip_names() const {
    std::vector<std::string> out;
    auto add = [&](const std::string& c) {
        if (!c.empty() && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    for (const ControllerState& s : states) {
        add(s.clip);
        for (const BlendPoint& p : s.points) add(p.clip);
    }
    for (const ControllerLayer& l : layers)
        if (l.machine)
            for (const std::string& n : l.machine->clip_names()) add(n);
    return out;
}

Result<std::shared_ptr<const Controller>> Controller::from_json(const json::Value& doc, std::string asset_name) {
    return parse_machine(doc, std::move(asset_name), nullptr);
}

namespace {
Result<std::shared_ptr<const Controller>> parse_machine(const json::Value& doc, std::string asset_name,
                                                        const std::vector<ControllerParam>* inherited) {
    const bool in_layer = inherited != nullptr;
    if (!doc.is_object()) return make_error(ErrorCode::ParseError, "animation controller: expected an object");
    if (const json::Value* v = doc.find("version")) {
        if (!v->is_number() || v->as_int(0) < 1)
            return make_error(ErrorCode::ParseError, "animation controller: \"version\" must be a positive integer");
        if (v->as_int(0) > static_cast<std::int64_t>(kControllerVersion))
            return make_error(ErrorCode::Unsupported,
                              std::format("animation controller: version {} is newer than this engine supports ({})",
                                          v->as_int(0), kControllerVersion));
    }
    auto c = std::make_shared<Controller>();
    c->name = std::move(asset_name);
    Status st;

    // Parameters.
    const json::Value* params = nullptr;
    if (in_layer) {
        for (const std::string_view key : {"parameters", "layers", "root_node", "version"})
            if (doc.find(key))
                return fail("document", std::format("\"{}\" is not allowed in a layer (layers share the controller's)", key));
        c->parameters = *inherited;
    }
    if (st = read_array(doc, "parameters", "document", Controller::kMaxParameters, params); !st)
        return std::move(st).error();
    for (std::size_t i = 0; params && i < params->size(); ++i) {
        const json::Value& pv = (*params)[i];
        const std::string where = std::format("parameters[{}]", i);
        if (!pv.is_object()) return fail(where, "a parameter must be an object");
        ControllerParam p;
        if (st = read_string(pv, "name", where, p.name, true); !st) return std::move(st).error();
        if (c->find_parameter(p.name) >= 0) return fail(where, "duplicate parameter \"" + p.name + "\"");
        std::string type = "float";
        if (st = read_string(pv, "type", where, type, false); !st) return std::move(st).error();
        const auto tn = std::find_if(kParamTypes.begin(), kParamTypes.end(),
                                      [&](const ParamTypeName& t) { return t.name == type; });
        if (tn == kParamTypes.end())
            return fail(where, "unknown parameter type \"" + type + "\" (float, int, bool or trigger)");
        p.type = tn->type;
        if (const json::Value* d = pv.find("default")) {
            if (p.type == ParamType::Bool || p.type == ParamType::Trigger) {
                if (!d->is_bool()) return fail(where, "\"default\" must be true or false");
                p.default_value = d->as_bool() ? 1.0f : 0.0f;
            } else {
                if (st = read_number(pv, "default", where, p.default_value); !st) return std::move(st).error();
                if (p.type == ParamType::Int) p.default_value = std::round(p.default_value);
            }
        }
        c->parameters.push_back(std::move(p));
    }

    auto numeric_param = [&](const std::string& n, const std::string& where) -> Status {
        const std::int32_t idx = c->find_parameter(n);
        if (idx < 0) return fail(where, "unknown parameter \"" + n + "\"");
        if (!is_numeric(c->parameters[static_cast<std::size_t>(idx)].type))
            return fail(where, "parameter \"" + n + "\" must be a float or int");
        return {};
    };

    // States.
    const json::Value* states = nullptr;
    if (st = read_array(doc, "states", "document", Controller::kMaxStates, states); !st) return std::move(st).error();
    if (!states || states->size() == 0) return fail("document", "a controller needs at least one state");
    for (std::size_t i = 0; i < states->size(); ++i) {
        const json::Value& sv = (*states)[i];
        const std::string where = std::format("states[{}]", i);
        if (!sv.is_object()) return fail(where, "a state must be an object");
        ControllerState s;
        if (st = read_string(sv, "name", where, s.name, true); !st) return std::move(st).error();
        if (s.name == "*") return fail(where, "\"*\" is reserved for any-state transitions");
        if (c->find_state(s.name) >= 0) return fail(where, "duplicate state \"" + s.name + "\"");
        const json::Value* clip = sv.find("clip");
        const json::Value* b1 = sv.find("blend1d");
        const json::Value* b2 = sv.find("blend2d");
        const int motions = (clip ? 1 : 0) + (b1 ? 1 : 0) + (b2 ? 1 : 0);
        if (motions > 1 || (motions == 0 && !in_layer))
            return fail(where, "a state needs exactly one of \"clip\", \"blend1d\" or \"blend2d\"");
        if (motions == 0) {
            s.motion = MotionType::Clip; // empty layer state: no clip, no influence
        } else if (clip) {
            s.motion = MotionType::Clip;
            if (st = read_string(sv, "clip", where, s.clip, true); !st) return std::move(st).error();
        } else {
            const bool two_d = b2 != nullptr;
            const json::Value& bv = two_d ? *b2 : *b1;
            const std::string bw = where + (two_d ? ".blend2d" : ".blend1d");
            if (!bv.is_object()) return fail(bw, "a blend space must be an object");
            s.motion = two_d ? MotionType::Blend2D : MotionType::Blend1D;
            if (two_d) {
                if (st = read_string(bv, "x", bw, s.param_x, true); !st) return std::move(st).error();
                if (st = read_string(bv, "y", bw, s.param_y, true); !st) return std::move(st).error();
                if (st = numeric_param(s.param_y, bw); !st) return std::move(st).error();
            } else {
                if (st = read_string(bv, "parameter", bw, s.param_x, true); !st) return std::move(st).error();
            }
            if (st = numeric_param(s.param_x, bw); !st) return std::move(st).error();
            const json::Value* pts = nullptr;
            if (st = read_array(bv, "points", bw, Controller::kMaxPointsPerState, pts); !st) return std::move(st).error();
            if (!pts || pts->size() == 0) return fail(bw, "a blend space needs at least one point");
            for (std::size_t k = 0; k < pts->size(); ++k) {
                const json::Value& kv = (*pts)[k];
                const std::string pw = std::format("{}.points[{}]", bw, k);
                if (!kv.is_object()) return fail(pw, "a blend point must be an object");
                BlendPoint bp;
                if (st = read_string(kv, "clip", pw, bp.clip, true); !st) return std::move(st).error();
                const json::Value* at = kv.find("at");
                if (!at) return fail(pw, "missing \"at\"");
                if (two_d) {
                    if (!at->is_array() || at->size() != 2 || !(*at)[0].is_number() || !(*at)[1].is_number())
                        return fail(pw, "\"at\" must be [x, y]");
                    bp.at = {(*at)[0].as_float(), (*at)[1].as_float()};
                } else {
                    if (!at->is_number()) return fail(pw, "\"at\" must be a number");
                    bp.at = {at->as_float(), 0.0f};
                }
                if (!finite(bp.at.x) || !finite(bp.at.y)) return fail(pw, "\"at\" must be finite");
                if (st = read_number(kv, "speed", pw, bp.speed); !st) return std::move(st).error();
                if (bp.speed < 0) return fail(pw, "\"speed\" must be >= 0");
                s.points.push_back(std::move(bp));
            }
            if (!two_d) {
                std::stable_sort(s.points.begin(), s.points.end(),
                                 [](const BlendPoint& a, const BlendPoint& b) { return a.at.x < b.at.x; });
                for (std::size_t k = 1; k < s.points.size(); ++k)
                    if (s.points[k].at.x == s.points[k - 1].at.x)
                        return fail(bw, std::format("two points share the threshold {}", s.points[k].at.x));
            }
        }
        if (st = read_number(sv, "speed", where, s.speed); !st) return std::move(st).error();
        if (s.speed < 0) return fail(where, "\"speed\" must be >= 0");
        if (st = read_string(sv, "speed_parameter", where, s.speed_parameter, false); !st)
            return std::move(st).error();
        if (!s.speed_parameter.empty())
            if (st = numeric_param(s.speed_parameter, where); !st) return std::move(st).error();
        if (const json::Value* l = sv.find("loop")) {
            if (!l->is_bool()) return fail(where, "\"loop\" must be true or false");
            s.loop = l->as_bool();
        }
        if (const json::Value* rm = sv.find("root_motion")) {
            if (!rm->is_bool()) return fail(where, "\"root_motion\" must be true or false");
            s.root_motion = rm->as_bool();
            if (s.root_motion && in_layer) return fail(where, "root motion is only taken from the base layer");
        }
        c->states.push_back(std::move(s));
    }
    if (const json::Value* d = doc.find("default_state")) {
        if (!d->is_string()) return fail("document", "\"default_state\" must be a string");
        c->default_state = c->find_state(d->as_string());
        if (c->default_state < 0) return fail("document", "unknown default_state \"" + d->as_string() + "\"");
    }

    if (const json::Value* rn = doc.find("root_node")) {
        if (!rn->is_string() || rn->as_string().empty() || rn->as_string().size() > 256)
            return fail("document", "\"root_node\" must be a non-empty node name");
        c->root_node = rn->as_string();
    }

    // Transitions.
    const json::Value* trans = nullptr;
    if (st = read_array(doc, "transitions", "document", Controller::kMaxTransitions, trans); !st) return std::move(st).error();
    for (std::size_t i = 0; trans && i < trans->size(); ++i) {
        const json::Value& tv = (*trans)[i];
        const std::string where = std::format("transitions[{}]", i);
        if (!tv.is_object()) return fail(where, "a transition must be an object");
        ControllerTransition t;
        std::string from, to;
        if (st = read_string(tv, "from", where, from, true); !st) return std::move(st).error();
        if (st = read_string(tv, "to", where, to, true); !st) return std::move(st).error();
        if (from != "*") {
            t.from = c->find_state(from);
            if (t.from < 0) return fail(where, "unknown state \"" + from + "\" in \"from\"");
        }
        t.to = c->find_state(to);
        if (t.to < 0) return fail(where, "unknown state \"" + to + "\" in \"to\"");
        if (st = read_number(tv, "duration", where, t.duration); !st) return std::move(st).error();
        if (t.duration < 0) return fail(where, "\"duration\" must be >= 0");
        if (tv.find("exit_time")) {
            if (st = read_number(tv, "exit_time", where, t.exit_time); !st) return std::move(st).error();
            if (t.exit_time < 0) return fail(where, "\"exit_time\" must be >= 0");
            if (t.from >= 0 && !c->states[static_cast<std::size_t>(t.from)].loop && t.exit_time > 1)
                return fail(where, "\"exit_time\" above 1 never fires from a non-looping state");
        }
        if (st = read_number(tv, "offset", where, t.offset); !st) return std::move(st).error();
        if (t.offset < 0 || t.offset > 1) return fail(where, "\"offset\" must be in [0, 1]");
        if (const json::Value* a = tv.find("allow_self")) {
            if (!a->is_bool()) return fail(where, "\"allow_self\" must be true or false");
            t.allow_self = a->as_bool();
        }
        const json::Value* conds = nullptr;
        if (st = read_array(tv, "conditions", where, Controller::kMaxConditions, conds); !st) return std::move(st).error();
        for (std::size_t k = 0; conds && k < conds->size(); ++k) {
            const json::Value& cv = (*conds)[k];
            const std::string cw = std::format("{}.conditions[{}]", where, k);
            if (!cv.is_object()) return fail(cw, "a condition must be an object");
            Condition cond;
            if (st = read_string(cv, "parameter", cw, cond.parameter, true); !st) return std::move(st).error();
            cond.index = c->find_parameter(cond.parameter);
            if (cond.index < 0) return fail(cw, "unknown parameter \"" + cond.parameter + "\"");
            const ParamType pt = c->parameters[static_cast<std::size_t>(cond.index)].type;
            std::string op;
            if (st = read_string(cv, "op", cw, op, false); !st) return std::move(st).error();
            if (op.empty()) {
                if (pt == ParamType::Trigger) op = "trigger";
                else if (pt == ParamType::Bool) op = "true";
                else return fail(cw, "numeric conditions need an \"op\" (>, >=, <, <=, == or !=)");
            }
            const auto on =
                std::find_if(kOps.begin(), kOps.end(), [&](const OpName& o) { return o.name == op; });
            if (on == kOps.end()) return fail(cw, "unknown op \"" + op + "\"");
            cond.op = on->op;
            const bool boolean_op = cond.op == ConditionOp::True || cond.op == ConditionOp::False;
            if (pt == ParamType::Trigger && cond.op != ConditionOp::Trigger)
                return fail(cw, "trigger parameters only support op \"trigger\"");
            if (pt != ParamType::Trigger && cond.op == ConditionOp::Trigger)
                return fail(cw, "op \"trigger\" needs a trigger parameter");
            if (pt == ParamType::Bool && !boolean_op) return fail(cw, "bool parameters support op \"true\" or \"false\"");
            if (is_numeric(pt)) {
                if (boolean_op) return fail(cw, "numeric parameters need a comparison op");
                if (!cv.find("value")) return fail(cw, "missing \"value\"");
                if (st = read_number(cv, "value", cw, cond.value); !st) return std::move(st).error();
            }
            t.conditions.push_back(std::move(cond));
        }
        if (t.conditions.empty() && t.exit_time < 0)
            return fail(where, "a transition without conditions needs an \"exit_time\"");
        if (t.from < 0 && t.conditions.empty())
            return fail(where, "an any-state (\"*\") transition needs at least one condition");
        if (t.allow_self && t.from >= 0) return fail(where, "\"allow_self\" only applies to \"*\" transitions");
        c->transitions.push_back(std::move(t));
    }

    // Layers.
    const json::Value* layers = nullptr;
    if (st = read_array(doc, "layers", "document", Controller::kMaxLayers, layers); !st) return std::move(st).error();
    for (std::size_t i = 0; layers && i < layers->size(); ++i) {
        const json::Value& lv = (*layers)[i];
        const std::string where = std::format("layers[{}]", i);
        if (!lv.is_object()) return fail(where, "a layer must be an object");
        ControllerLayer l;
        if (st = read_string(lv, "name", where, l.name, true); !st) return std::move(st).error();
        if (c->find_layer(l.name) >= 0) return fail(where, "duplicate layer \"" + l.name + "\"");
        if (st = read_number(lv, "weight", where, l.weight); !st) return std::move(st).error();
        if (l.weight < 0 || l.weight > 1) return fail(where, "\"weight\" must be in [0, 1]");
        if (st = read_string(lv, "weight_parameter", where, l.weight_parameter, false); !st) return std::move(st).error();
        if (!l.weight_parameter.empty())
            if (st = numeric_param(l.weight_parameter, where); !st) return std::move(st).error();
        std::string blend = "override";
        if (st = read_string(lv, "blend", where, blend, false); !st) return std::move(st).error();
        if (blend == "additive") l.blend = LayerBlend::Additive;
        else if (blend != "override") return fail(where, "unknown blend \"" + blend + "\" (override or additive)");
        std::string ref = "first_frame";
        if (st = read_string(lv, "reference", where, ref, false); !st) return std::move(st).error();
        if (ref == "rest") l.reference = AdditiveReference::Rest;
        else if (ref != "first_frame") return fail(where, "unknown reference \"" + ref + "\" (first_frame or rest)");
        if (lv.find("reference") && l.blend != LayerBlend::Additive)
            return fail(where, "\"reference\" only applies to additive layers");
        if (st = read_names(lv, "mask", where, l.mask); !st) return std::move(st).error();
        if (st = read_names(lv, "exclude", where, l.exclude); !st) return std::move(st).error();
        if (const json::Value* sv = lv.find("sync")) {
            if (!sv->is_bool()) return fail(where, "\"sync\" must be true or false");
            l.sync = sv->as_bool();
        }
        if (const json::Value* mw = lv.find("mask_weights")) {
            if (!mw->is_object()) return fail(where, "\"mask_weights\" must be an object of node name -> weight");
            if (mw->as_object().size() > Controller::kMaxMaskNodes)
                return fail(where, std::format("\"mask_weights\" has more than {} entries", Controller::kMaxMaskNodes));
            for (const auto& [node, v] : mw->as_object()) {
                if (node.empty() || node.size() > 256) return fail(where, "\"mask_weights\" keys must be non-empty node names");
                if (!v.is_number() || !std::isfinite(v.as_number()) || v.as_number() < 0.0 || v.as_number() > 1.0)
                    return fail(where, std::format("\"mask_weights\".\"{}\" must be a number from 0 to 1", node));
                l.mask_weights.emplace_back(node, static_cast<float>(v.as_number()));
            }
        }
        {
            std::vector<std::string_view> seen;
            auto dup = [&](std::string_view nm) {
                if (std::find(seen.begin(), seen.end(), nm) != seen.end()) return true;
                seen.push_back(nm);
                return false;
            };
            for (const std::string& nm : l.mask)
                if (dup(nm)) return fail(where, std::format("node \"{}\" is listed twice in mask/exclude/mask_weights", nm));
            for (const std::string& nm : l.exclude)
                if (dup(nm)) return fail(where, std::format("node \"{}\" is listed twice in mask/exclude/mask_weights", nm));
            for (const auto& [nm, w] : l.mask_weights)
                if (dup(nm)) return fail(where, std::format("node \"{}\" is listed twice in mask/exclude/mask_weights", nm));
        }
        json::Value machine = json::Value::object();
        for (const std::string_view key : {"default_state", "states", "transitions"})
            if (const json::Value* v = lv.find(key)) machine.set(std::string(key), *v);
        for (const std::string_view key : {"parameters", "layers", "root_node"})
            if (lv.find(key)) return fail(where, std::format("\"{}\" is not allowed in a layer", key));
        auto m = parse_machine(machine, c->name, &c->parameters);
        if (!m) return nest_error(std::move(m).error(), where);
        l.machine = std::move(*m);
        c->layers.push_back(std::move(l));
    }
    return std::shared_ptr<const Controller>(std::move(c));
}
} // namespace

Result<std::shared_ptr<const Controller>> Controller::parse(std::string_view text, std::string asset_name) {
    auto doc = json::parse(text, {.max_depth = 16, .max_bytes = 16u * 1024u * 1024u, .max_string_bytes = 1u << 16});
    if (!doc) return make_error(ErrorCode::ParseError, "animation controller: " + doc.error().message);
    return from_json(*doc, std::move(asset_name));
}

json::Value Controller::to_json() const {
    json::Value doc = json::Value::object();
    doc.set("version", kControllerVersion);
    json::Value ps = json::Value::array();
    for (const ControllerParam& p : parameters) {
        json::Value o = json::Value::object();
        o.set("name", p.name);
        o.set("type", std::string(param_type_name(p.type)));
        if (p.type == ParamType::Bool || p.type == ParamType::Trigger) o.set("default", p.default_value != 0.0f);
        else o.set("default", p.default_value);
        ps.push(std::move(o));
    }
    doc.set("parameters", std::move(ps));
    doc.set("default_state", states.empty() ? std::string() : states[static_cast<std::size_t>(default_state)].name);
    json::Value ss = json::Value::array();
    for (const ControllerState& s : states) {
        json::Value o = json::Value::object();
        o.set("name", s.name);
        if (s.motion == MotionType::Clip) {
            if (!s.clip.empty()) o.set("clip", s.clip); // empty: an empty layer state
        } else {
            json::Value b = json::Value::object();
            const bool two_d = s.motion == MotionType::Blend2D;
            if (two_d) {
                b.set("x", s.param_x);
                b.set("y", s.param_y);
            } else {
                b.set("parameter", s.param_x);
            }
            json::Value pts = json::Value::array();
            for (const BlendPoint& bp : s.points) {
                json::Value po = json::Value::object();
                po.set("clip", bp.clip);
                if (two_d) {
                    json::Value at = json::Value::array();
                    at.push(bp.at.x);
                    at.push(bp.at.y);
                    po.set("at", std::move(at));
                } else {
                    po.set("at", bp.at.x);
                }
                if (bp.speed != 1.0f) po.set("speed", bp.speed);
                pts.push(std::move(po));
            }
            b.set("points", std::move(pts));
            o.set(two_d ? "blend2d" : "blend1d", std::move(b));
        }
        if (s.speed != 1.0f) o.set("speed", s.speed);
        if (!s.speed_parameter.empty()) o.set("speed_parameter", s.speed_parameter);
        if (!s.loop) o.set("loop", false);
        if (s.root_motion) o.set("root_motion", true);
        ss.push(std::move(o));
    }
    doc.set("states", std::move(ss));
    if (!root_node.empty()) doc.set("root_node", root_node);
    json::Value ts = json::Value::array();
    for (const ControllerTransition& t : transitions) {
        json::Value o = json::Value::object();
        o.set("from", t.from < 0 ? std::string("*") : states[static_cast<std::size_t>(t.from)].name);
        o.set("to", states[static_cast<std::size_t>(t.to)].name);
        o.set("duration", t.duration);
        if (t.exit_time >= 0) o.set("exit_time", t.exit_time);
        if (t.offset != 0) o.set("offset", t.offset);
        if (t.allow_self) o.set("allow_self", true);
        json::Value cs = json::Value::array();
        for (const Condition& cond : t.conditions) {
            json::Value co = json::Value::object();
            co.set("parameter", cond.parameter);
            co.set("op", std::string(condition_op_name(cond.op)));
            const ParamType pt = parameters[static_cast<std::size_t>(cond.index)].type;
            if (is_numeric(pt)) co.set("value", cond.value);
            cs.push(std::move(co));
        }
        o.set("conditions", std::move(cs));
        ts.push(std::move(o));
    }
    doc.set("transitions", std::move(ts));
    if (!layers.empty()) {
        json::Value ls = json::Value::array();
        for (const ControllerLayer& l : layers) {
            json::Value o = json::Value::object();
            o.set("name", l.name);
            if (l.weight != 1.0f) o.set("weight", l.weight);
            if (!l.weight_parameter.empty()) o.set("weight_parameter", l.weight_parameter);
            o.set("blend", std::string(layer_blend_name(l.blend)));
            if (l.blend == LayerBlend::Additive && l.reference == AdditiveReference::Rest) o.set("reference", "rest");
            auto names = [](const std::vector<std::string>& v) {
                json::Value a = json::Value::array();
                for (const std::string& n : v) a.push(n);
                return a;
            };
            if (!l.mask.empty()) o.set("mask", names(l.mask));
            if (!l.exclude.empty()) o.set("exclude", names(l.exclude));
            if (l.sync) o.set("sync", true);
            if (!l.mask_weights.empty()) {
                json::Value mw = json::Value::object();
                for (const auto& [nm, w] : l.mask_weights) mw.set(nm, w);
                o.set("mask_weights", std::move(mw));
            }
            if (l.machine) {
                const json::Value m = l.machine->to_json();
                for (const char* key : {"default_state", "states", "transitions"})
                    if (const json::Value* v = m.find(key)) o.set(key, *v);
            }
            ls.push(std::move(o));
        }
        doc.set("layers", std::move(ls));
    }
    return doc;
}

void blend_weights_1d(std::span<const float> t, float x, std::span<float> w) noexcept {
    const std::size_t n = std::min(t.size(), w.size());
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = 0;
    if (n == 0) return;
    if (!std::isfinite(x) || x <= t[0]) {
        w[0] = 1;
        return;
    }
    if (x >= t[n - 1]) {
        w[n - 1] = 1;
        return;
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (x <= t[i + 1]) {
            const float span = t[i + 1] - t[i];
            const float f = span > 0 ? (x - t[i]) / span : 1.0f;
            w[i] = 1.0f - f;
            w[i + 1] = f;
            return;
        }
    }
    w[n - 1] = 1;
}

void blend_weights_2d(std::span<const math::Vec2> pts, math::Vec2 p, std::span<float> w) noexcept {
    const std::size_t n = std::min(pts.size(), w.size());
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = 0;
    if (n == 0) return;
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) p = {0, 0};
    float total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        float h = 1.0f;
        const float px = p.x - pts[i].x, py = p.y - pts[i].y;
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const float dx = pts[j].x - pts[i].x, dy = pts[j].y - pts[i].y;
            const float len2 = dx * dx + dy * dy;
            if (len2 <= 1e-12f) continue; // coincident points share the weight
            h = std::min(h, 1.0f - (px * dx + py * dy) / len2);
        }
        w[i] = std::max(h, 0.0f);
        total += w[i];
    }
    if (total > 1e-12f) {
        for (std::size_t i = 0; i < n; ++i) w[i] /= total;
        return;
    }
    // Degenerate (cannot happen for distinct points, kept for safety): nearest point wins.
    std::size_t best = 0;
    float best_d = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < n; ++i) {
        const float dx = p.x - pts[i].x, dy = p.y - pts[i].y;
        if (dx * dx + dy * dy < best_d) {
            best_d = dx * dx + dy * dy;
            best = i;
        }
    }
    w[best] = 1;
}

} // namespace victus::animation

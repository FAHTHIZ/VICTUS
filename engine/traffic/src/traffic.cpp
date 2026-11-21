// SPDX-License-Identifier: Apache-2.0
// Traffic (ADR-0167, GAP-22). See traffic.hpp for the model and file format.
#include "victus/traffic/traffic.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace victus::traffic {

using math::Vec3;

namespace {

constexpr Vec3 kUp{0, 1, 0};
constexpr float kLateralAccel = 3.0f; // m/s^2 used to cap connector speeds by curvature
constexpr float kMaxSubstep = 0.1f;   // s

Vec3 flat(Vec3 v) noexcept { return {v.x, 0, v.z}; }

Vec3 safe_normalize(Vec3 v, Vec3 fallback) noexcept {
    const float l = math::length(v);
    return l > 1e-6f ? v * (1.0f / l) : fallback;
}

std::uint64_t mix(std::uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

void finish_lane(Lane& lane) {
    lane.distance.assign(lane.points.size(), 0.0f);
    float acc = 0;
    for (std::size_t i = 1; i < lane.points.size(); ++i) {
        acc += math::length(lane.points[i] - lane.points[i - 1]);
        lane.distance[i] = acc;
    }
    lane.length = acc;
}

Vec3 cubic(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) noexcept {
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

struct Incident {
    std::uint32_t road;
    Vec3 out; // horizontal unit direction from the node along the road
};

} // namespace

// ---- road network --------------------------------------------------------------------------

RoadNetwork grid_network(std::uint32_t columns, std::uint32_t rows, float block, std::uint32_t lanes, Vec3 origin,
                         float speed_limit) {
    RoadNetwork net;
    columns = std::max<std::uint32_t>(columns, 1);
    rows = std::max<std::uint32_t>(rows, 1);
    const float cx = 0.5f * static_cast<float>(columns - 1), cz = 0.5f * static_cast<float>(rows - 1);
    for (std::uint32_t j = 0; j < rows; ++j)
        for (std::uint32_t i = 0; i < columns; ++i)
            net.add_node(origin + Vec3{(static_cast<float>(i) - cx) * block, 0, (static_cast<float>(j) - cz) * block});
    auto id = [&](std::uint32_t i, std::uint32_t j) { return j * columns + i; };
    for (std::uint32_t j = 0; j < rows; ++j)
        for (std::uint32_t i = 0; i < columns; ++i) {
            if (i + 1 < columns) net.add_road({id(i, j), id(i + 1, j), lanes, lanes, 3.5f, speed_limit});
            if (j + 1 < rows) net.add_road({id(i, j), id(i, j + 1), lanes, lanes, 3.5f, speed_limit});
        }
    return net;
}

Result<RoadNetwork> parse_road_network(const json::Value& doc) {
    if (!doc.is_object()) return make_error(ErrorCode::ParseError, "road network: expected a JSON object");
    if (doc["format"].as_string_or("") != "victus.roads")
        return make_error(ErrorCode::ParseError, "road network: format must be \"victus.roads\"");
    if (doc["version"].as_int(0) != 1)
        return make_error(ErrorCode::VersionMismatch, "road network: unsupported version " +
                                                          std::to_string(doc["version"].as_int(0)));
    RoadNetwork net;
    const json::Value& nodes = doc["nodes"];
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const json::Value& p = nodes[i]["position"];
        if (p.size() != 3) return make_error(ErrorCode::ParseError, "road network: node " + std::to_string(i) + " needs a 3-number position");
        net.add_node({p[0].as_float(), p[1].as_float(), p[2].as_float()});
    }
    const json::Value& roads = doc["roads"];
    for (std::size_t i = 0; i < roads.size(); ++i) {
        const json::Value& r = roads[i];
        Road road;
        const std::int64_t a = r["a"].as_int(-1), b = r["b"].as_int(-1);
        if (a < 0 || b < 0) return make_error(ErrorCode::ParseError, "road network: road " + std::to_string(i) + " needs nodes a and b");
        road.a = static_cast<std::uint32_t>(a);
        road.b = static_cast<std::uint32_t>(b);
        road.lanes = static_cast<std::uint32_t>(std::max<std::int64_t>(r["lanes"].as_int(1), 0));
        road.lanes_back = static_cast<std::uint32_t>(std::max<std::int64_t>(r["lanes_back"].as_int(1), 0));
        road.lane_width = r["lane_width"].as_float(3.5f);
        road.speed_limit = r["speed_limit"].as_float(13.9f);
        net.add_road(road);
    }
    if (auto problems = validate(net); !problems.empty())
        return make_error(ErrorCode::InvalidArgument, "road network: " + problems.front());
    return net;
}

Result<RoadNetwork> parse_road_network(std::string_view text) {
    auto doc = json::parse(text);
    if (!doc) return doc.error();
    return parse_road_network(doc.value());
}

json::Value road_network_to_json(const RoadNetwork& net) {
    json::Value doc = json::Value::object();
    doc.set("format", "victus.roads");
    doc.set("version", 1);
    json::Value nodes = json::Value::array();
    for (const RoadNode& n : net.nodes) {
        json::Value p = json::Value::array();
        p.push(n.position.x);
        p.push(n.position.y);
        p.push(n.position.z);
        json::Value o = json::Value::object();
        o.set("position", std::move(p));
        nodes.push(std::move(o));
    }
    doc.set("nodes", std::move(nodes));
    json::Value roads = json::Value::array();
    for (const Road& r : net.roads) {
        json::Value o = json::Value::object();
        o.set("a", static_cast<std::int64_t>(r.a));
        o.set("b", static_cast<std::int64_t>(r.b));
        o.set("lanes", static_cast<std::int64_t>(r.lanes));
        o.set("lanes_back", static_cast<std::int64_t>(r.lanes_back));
        o.set("lane_width", r.lane_width);
        o.set("speed_limit", r.speed_limit);
        roads.push(std::move(o));
    }
    doc.set("roads", std::move(roads));
    return doc;
}

std::vector<std::string> validate(const RoadNetwork& net) {
    std::vector<std::string> out;
    const auto n = static_cast<std::uint32_t>(net.nodes.size());
    for (std::size_t i = 0; i < net.roads.size(); ++i) {
        const Road& r = net.roads[i];
        const std::string name = "road " + std::to_string(i);
        if (r.a >= n || r.b >= n) {
            out.push_back(name + " refers to a missing node");
            continue;
        }
        if (r.a == r.b) out.push_back(name + " starts and ends at the same node");
        else if (math::length(flat(net.nodes[r.b].position - net.nodes[r.a].position)) < 1.0f)
            out.push_back(name + " is shorter than 1 m");
        if (r.lanes + r.lanes_back == 0) out.push_back(name + " has no lanes");
        if (r.lanes > 8 || r.lanes_back > 8) out.push_back(name + " has more than 8 lanes in one direction");
        if (!(r.lane_width > 0.5f)) out.push_back(name + " lane width must be above 0.5 m");
        if (!(r.speed_limit > 0)) out.push_back(name + " speed limit must be positive");
    }
    for (std::size_t i = 0; i < net.nodes.size(); ++i) {
        const Vec3 p = net.nodes[i].position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            out.push_back("node " + std::to_string(i) + " has a non-finite position");
    }
    return out;
}

// ---- lane graph ----------------------------------------------------------------------------

Vec3 LaneGraph::point_at(std::uint32_t lane, float s, Vec3* tangent) const {
    if (lane >= lanes.size() || lanes[lane].points.empty()) {
        if (tangent) *tangent = {0, 0, 1};
        return {};
    }
    const Lane& l = lanes[lane];
    if (l.points.size() == 1) {
        if (tangent) *tangent = {0, 0, 1};
        return l.points[0];
    }
    s = std::clamp(s, 0.0f, l.length);
    auto it = std::upper_bound(l.distance.begin(), l.distance.end(), s);
    std::size_t i = static_cast<std::size_t>(it - l.distance.begin());
    i = std::clamp<std::size_t>(i, 1, l.points.size() - 1);
    const float d0 = l.distance[i - 1], d1 = l.distance[i];
    const float t = d1 > d0 ? (s - d0) / (d1 - d0) : 0.0f;
    if (tangent) *tangent = safe_normalize(l.points[i] - l.points[i - 1], {0, 0, 1});
    return l.points[i - 1] + (l.points[i] - l.points[i - 1]) * t;
}

Light LaneGraph::light(std::uint32_t lane, double t) const {
    if (lane >= lanes.size()) return Light::Green;
    const Lane& l = lanes[lane];
    if (!l.connector || l.signal >= signals.size()) return Light::Green;
    const Signal& sig = signals[l.signal];
    const double cycle = sig.cycle();
    if (!(cycle > 0)) return Light::Green;
    double local = std::fmod(t + static_cast<double>(sig.offset), cycle);
    if (local < 0) local += cycle;
    const double slot = static_cast<double>(sig.green + sig.amber + sig.all_red);
    const auto current = std::min<std::uint32_t>(static_cast<std::uint32_t>(local / slot), sig.phases - 1);
    if (current != l.phase) return Light::Red;
    const double within = local - static_cast<double>(current) * slot;
    if (within < sig.green) return Light::Green;
    if (within < sig.green + sig.amber) return Light::Amber;
    return Light::Red;
}

float LaneGraph::total_length() const {
    float t = 0;
    for (const Lane& l : lanes) t += l.length;
    return t;
}

std::uint32_t LaneGraph::nearest_lane(Vec3 p, float* s_out) const {
    std::uint32_t best = kNone;
    float best_d2 = std::numeric_limits<float>::infinity(), best_s = 0;
    for (std::uint32_t li = 0; li < lanes.size(); ++li) {
        const Lane& l = lanes[li];
        if (l.connector) continue;
        for (std::size_t i = 1; i < l.points.size(); ++i) {
            const Vec3 a = l.points[i - 1], ab = l.points[i] - a;
            const float len2 = math::dot(ab, ab);
            const float t = len2 > 0 ? std::clamp(math::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const Vec3 q = a + ab * t;
            const float d2 = math::dot(p - q, p - q);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = li;
                best_s = l.distance[i - 1] + t * (l.distance[i] - l.distance[i - 1]);
            }
        }
    }
    if (s_out) *s_out = best_s;
    return best;
}

LaneGraph build_lane_graph(const RoadNetwork& net, const LaneGraphSettings& settings) {
    VICTUS_PROFILE_SCOPE("traffic.build_lane_graph");
    LaneGraph g;
    g.settings = settings;
    const auto node_count = static_cast<std::uint32_t>(net.nodes.size());
    const float side = settings.left_hand ? -1.0f : 1.0f;

    // Usable roads and each node's incident roads.
    std::vector<std::vector<Incident>> incident(node_count);
    std::vector<bool> usable(net.roads.size(), false);
    for (std::uint32_t ri = 0; ri < net.roads.size(); ++ri) {
        const Road& r = net.roads[ri];
        if (r.a >= node_count || r.b >= node_count || r.a == r.b || r.lanes + r.lanes_back == 0) continue;
        const Vec3 d = flat(net.nodes[r.b].position - net.nodes[r.a].position);
        if (math::length(d) < 1.0f) continue;
        usable[ri] = true;
        const Vec3 u = math::normalize(d);
        incident[r.a].push_back({ri, u});
        incident[r.b].push_back({ri, u * -1.0f});
    }
    // Stable angular order around each node (signal phases follow it).
    for (auto& list : incident)
        std::sort(list.begin(), list.end(), [](const Incident& x, const Incident& y) {
            const float ax = std::atan2(x.out.x, x.out.z), ay = std::atan2(y.out.x, y.out.z);
            return ax != ay ? ax < ay : x.road < y.road;
        });

    // Clearance radius per node: half the widest incident road plus a kerb margin.
    std::vector<float> radius(node_count, 0.0f);
    for (std::uint32_t n = 0; n < node_count; ++n) {
        float widest = 0;
        for (const Incident& inc : incident[n]) {
            const Road& r = net.roads[inc.road];
            widest = std::max(widest, 0.5f * static_cast<float>(r.lanes + r.lanes_back) * r.lane_width);
        }
        if (incident[n].size() >= 3) radius[n] = settings.intersection_radius > 0 ? settings.intersection_radius : widest + 1.5f;
        else if (incident[n].size() == 2) radius[n] = widest + 0.5f;
        else radius[n] = widest; // dead end: room for a U-turn
    }

    // Driving lanes. lane_ids[road][dir] lists lanes (kerb first); dir 0 = a->b, 1 = b->a.
    std::vector<std::array<std::vector<std::uint32_t>, 2>> lane_ids(net.roads.size());
    for (std::uint32_t ri = 0; ri < net.roads.size(); ++ri) {
        if (!usable[ri]) continue;
        const Road& r = net.roads[ri];
        for (int dir = 0; dir < 2; ++dir) {
            const std::uint32_t from = dir == 0 ? r.a : r.b, to = dir == 0 ? r.b : r.a;
            const std::uint32_t count = dir == 0 ? r.lanes : r.lanes_back;
            const std::uint32_t other = dir == 0 ? r.lanes_back : r.lanes;
            const Vec3 pa = net.nodes[from].position, pb = net.nodes[to].position;
            const float len = math::length(flat(pb - pa));
            float ra = radius[from], rb = radius[to];
            if (ra + rb > len - 1.0f) {
                const float k = std::max(len - 1.0f, 0.0f) / std::max(ra + rb, 1e-3f);
                ra *= k;
                rb *= k;
            }
            const Vec3 d3 = pb - pa;
            const Vec3 right = safe_normalize(math::cross(flat(d3), kUp), {1, 0, 0}) * side;
            const float divider = 0.5f * (static_cast<float>(other) - static_cast<float>(count)) * r.lane_width;
            for (std::uint32_t k = 0; k < count; ++k) {
                const float off = divider + (static_cast<float>(count - k) - 0.5f) * r.lane_width;
                Lane lane;
                lane.road = ri;
                lane.node = to;
                lane.index = k;
                lane.speed_limit = r.speed_limit;
                const Vec3 a0 = pa + d3 * (ra / len) + right * off;
                const Vec3 b0 = pb - d3 * (rb / len) + right * off;
                lane.points = {a0, b0};
                finish_lane(lane);
                lane_ids[ri][static_cast<std::size_t>(dir)].push_back(static_cast<std::uint32_t>(g.lanes.size()));
                g.lanes.push_back(std::move(lane));
            }
        }
    }

    // Signals at nodes with 3+ roads.
    std::vector<std::uint32_t> signal_of(node_count, kNone);
    for (std::uint32_t n = 0; n < node_count; ++n) {
        if (incident[n].size() < 3) continue;
        Signal s;
        s.node = n;
        s.phases = static_cast<std::uint32_t>(incident[n].size());
        s.green = std::max(settings.green, 0.1f);
        s.amber = std::max(settings.amber, 0.0f);
        s.all_red = std::max(settings.all_red, 0.0f);
        if (settings.stagger_signals)
            s.offset = static_cast<float>(static_cast<double>(mix(n + 1) % 100000) / 100000.0) * s.cycle();
        signal_of[n] = static_cast<std::uint32_t>(g.signals.size());
        g.signals.push_back(s);
    }

    // Connectors: from every lane arriving at a node to the lanes leaving on the other roads.
    auto arriving = [&](std::uint32_t road, std::uint32_t node) -> const std::vector<std::uint32_t>& {
        return lane_ids[road][net.roads[road].b == node ? 0 : 1];
    };
    auto leaving = [&](std::uint32_t road, std::uint32_t node) -> const std::vector<std::uint32_t>& {
        return lane_ids[road][net.roads[road].a == node ? 0 : 1];
    };
    const float spacing = std::max(settings.connector_spacing, 0.1f);
    for (std::uint32_t n = 0; n < node_count; ++n) {
        const auto& list = incident[n];
        const bool dead_end = list.size() == 1;
        for (std::uint32_t pi = 0; pi < list.size(); ++pi) {
            const std::uint32_t in_road = list[pi].road;
            for (const std::uint32_t in_lane : arriving(in_road, n)) {
                for (const Incident& outgoing : list) {
                    if (outgoing.road == in_road && !(dead_end && settings.u_turns_at_dead_ends)) continue;
                    const auto& outs = leaving(outgoing.road, n);
                    if (outs.empty()) continue;
                    const std::uint32_t idx = std::min<std::uint32_t>(g.lanes[in_lane].index, static_cast<std::uint32_t>(outs.size() - 1));
                    const std::uint32_t out_lane = outs[idx];
                    const Lane& li = g.lanes[in_lane];
                    const Lane& lo = g.lanes[out_lane];
                    const Vec3 p0 = li.points.back(), p3 = lo.points.front();
                    const Vec3 t0 = safe_normalize(li.points.back() - li.points.front(), {0, 0, 1});
                    const Vec3 t3 = safe_normalize(lo.points.back() - lo.points.front(), {0, 0, 1});
                    const float chord = math::length(p3 - p0);
                    const float cosang = std::clamp(math::dot(flat(t0), flat(t3)), -1.0f, 1.0f);
                    const bool u_turn = cosang < -0.9f;
                    const float h = u_turn ? std::max(chord, 3.0f) : 0.5f * chord;
                    const Vec3 c1 = p0 + t0 * h, c2 = p3 - t3 * h;
                    const float est = chord + (u_turn ? h * 1.5f : 0.0f);
                    const int steps = std::clamp(static_cast<int>(std::ceil(est / spacing)), 2, 256);
                    Lane c;
                    c.connector = true;
                    c.node = n;
                    c.index = li.index;
                    c.points.reserve(static_cast<std::size_t>(steps) + 1);
                    for (int s = 0; s <= steps; ++s)
                        c.points.push_back(cubic(p0, c1, c2, p3, static_cast<float>(s) / static_cast<float>(steps)));
                    finish_lane(c);
                    // Curvature speed cap: radius from the turn angle and the arc length.
                    const float angle = std::acos(cosang);
                    float limit = std::min(li.speed_limit, lo.speed_limit);
                    if (angle > 0.05f) limit = std::min(limit, std::sqrt(kLateralAccel * c.length / angle));
                    c.speed_limit = std::max(limit, 2.0f);
                    if (signal_of[n] != kNone) {
                        c.signal = signal_of[n];
                        c.phase = pi;
                    }
                    const auto ci = static_cast<std::uint32_t>(g.lanes.size());
                    c.prev = {in_lane};
                    c.next = {out_lane};
                    g.lanes.push_back(std::move(c));
                    g.lanes[in_lane].next.push_back(ci);
                    g.lanes[out_lane].prev.push_back(ci);
                }
            }
        }
    }
    return g;
}

std::vector<Vec3> lane_graph_lines(const LaneGraph& graph) {
    std::vector<Vec3> out;
    for (const Lane& l : graph.lanes)
        for (std::size_t i = 1; i < l.points.size(); ++i) {
            out.push_back(l.points[i - 1]);
            out.push_back(l.points[i]);
        }
    return out;
}

// ---- agents --------------------------------------------------------------------------------

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using C = TrafficAgentComponent;
        scene::Schema<C>("TrafficAgent", "Gameplay")
            .field(&C::max_speed, "max_speed", {.min = 0, .max = 100, .speed = 0.1f, .label = "Max Speed", .tooltip = "Desired speed on an open road, m/s"})
            .field(&C::acceleration, "acceleration", {.min = 0.1f, .max = 20, .speed = 0.05f, .tooltip = "Comfortable acceleration, m/s^2"})
            .field(&C::deceleration, "deceleration", {.min = 0.1f, .max = 20, .speed = 0.05f, .tooltip = "Comfortable braking, m/s^2"})
            .field(&C::time_headway, "time_headway", {.min = 0.1f, .max = 10, .speed = 0.05f, .label = "Time Headway", .tooltip = "Seconds of gap kept to the vehicle ahead"})
            .field(&C::min_gap, "min_gap", {.min = 0, .max = 50, .speed = 0.1f, .label = "Minimum Gap", .tooltip = "Bumper-to-bumper gap when stopped, m"})
            .field(&C::length, "length", {.min = 0.5f, .max = 30, .speed = 0.1f})
            .field(&C::seed, "seed", {.tooltip = "Turn choices; 0 uses the entity index"})
            .field(&C::enabled, "enabled")
            .field(&C::lane, "lane")
            .field(&C::distance, "distance")
            .field(&C::speed, "speed")
            .field(&C::next, "next")
            .field(&C::turns, "turns")
            .commit();
        using N = TrafficNetworkComponent;
        scene::Schema<N>("TrafficNetwork", "Gameplay")
            .field(&N::network, "network", {.tooltip = "Road network (.vroads) relative to the assets folder; empty generates a grid"})
            .field(&N::columns, "columns", {.min = 1, .max = 64, .tooltip = "Grid intersections across"})
            .field(&N::rows, "rows", {.min = 1, .max = 64, .tooltip = "Grid intersections deep"})
            .field(&N::block, "block", {.min = 10, .max = 1000, .speed = 1, .tooltip = "Metres between grid intersections"})
            .field(&N::lanes, "lanes", {.min = 1, .max = 4, .tooltip = "Grid lanes in each direction"})
            .field(&N::speed_limit, "speed_limit", {.min = 1, .max = 60, .speed = 0.1f, .label = "Speed Limit", .tooltip = "Grid roads, m/s (13.9 = 50 km/h)"})
            .field(&N::vehicles, "vehicles", {.min = 0, .max = 20000, .tooltip = "Agents spawned when play starts"})
            .field(&N::seed, "seed")
            .field(&N::left_hand, "left_hand", {.label = "Drive On Left"})
            .field(&N::green, "green", {.min = 1, .max = 120, .speed = 0.1f, .label = "Green Seconds", .tooltip = "Green time per approach at signalled intersections"})
            .commit();
    });
}

namespace {

TrafficWorld& traffic_world(scene::Scene& scene) {
    auto& w = scene.world();
    if (auto* t = w.resource<TrafficWorld>()) return *t;
    return w.set_resource<TrafficWorld>();
}

std::uint32_t choose_next(const LaneGraph& g, std::uint32_t lane, std::uint32_t seed, std::uint32_t turns) {
    if (lane >= g.lanes.size()) return kNone;
    const auto& next = g.lanes[lane].next;
    if (next.empty()) return kNone;
    if (next.size() == 1) return next[0];
    const std::uint64_t h = mix((static_cast<std::uint64_t>(seed) << 32) ^ (static_cast<std::uint64_t>(turns) * 0x9e3779b97f4a7c15ull) ^ lane);
    return next[static_cast<std::size_t>(h % next.size())];
}

struct Agent {
    Entity entity;
    TrafficAgentComponent* c;
    std::uint32_t seed;
};

struct Leader {
    float gap{std::numeric_limits<float>::infinity()};
    float speed{0};
    bool light{false};
};

} // namespace

void set_traffic_graph(scene::Scene& scene, std::shared_ptr<const LaneGraph> graph) {
    TrafficWorld& tw = traffic_world(scene);
    tw.graph = std::move(graph);
    tw.time = 0;
    tw.stats = {};
    // Agents placed on the previous graph re-attach to the nearest lane on the next update.
    scene.world().each<TrafficAgentComponent>([](TrafficAgentComponent& c) {
        c.lane = kNone;
        c.next = kNone;
    });
}

const LaneGraph* traffic_graph(const scene::Scene& scene) {
    const auto* tw = const_cast<scene::Scene&>(scene).world().resource<TrafficWorld>();
    return tw ? tw->graph.get() : nullptr;
}

const TrafficStats* traffic_stats(const scene::Scene& scene) {
    const auto* tw = const_cast<scene::Scene&>(scene).world().resource<TrafficWorld>();
    return tw ? &tw->stats : nullptr;
}

std::vector<Entity> spawn_traffic(scene::Scene& scene, std::uint32_t count, std::uint32_t seed, Entity parent) {
    register_components();
    std::vector<Entity> out;
    const LaneGraph* g = traffic_graph(scene);
    if (!g || count == 0) return out;
    std::vector<std::uint32_t> driving;
    float total = 0;
    for (std::uint32_t i = 0; i < g->lanes.size(); ++i)
        if (!g->lanes[i].connector && g->lanes[i].length > 1.0f) {
            driving.push_back(i);
            total += g->lanes[i].length;
        }
    if (driving.empty()) return out;
    Random rng(seed);
    const float step = total / static_cast<float>(count);
    float cursor = step * 0.5f * (0.5f + rng.next_float());
    std::size_t li = 0;
    float lane_start = 0;
    for (std::uint32_t k = 0; k < count; ++k) {
        while (li + 1 < driving.size() && cursor > lane_start + g->lanes[driving[li]].length) {
            lane_start += g->lanes[driving[li]].length;
            ++li;
        }
        const std::uint32_t lane = driving[li];
        const Entity e = scene.create("Traffic " + std::to_string(k), parent);
        TrafficAgentComponent c;
        c.seed = seed * 7919u + k + 1u;
        c.lane = lane;
        c.distance = std::clamp(cursor - lane_start, 0.0f, g->lanes[lane].length);
        c.max_speed = g->lanes[lane].speed_limit * (0.85f + 0.3f * rng.next_float());
        c.next = choose_next(*g, lane, c.seed, 0);
        Vec3 t;
        const Vec3 p = g->point_at(lane, c.distance - 0.5f * c.length, &t);
        if (auto* tr = scene.world().get<scene::Transform>(e)) {
            tr->position = p;
            tr->rotation = math::Quat::euler(0, std::atan2(t.x, t.z), 0);
        }
        scene.world().add<TrafficAgentComponent>(e, c);
        out.push_back(e);
        cursor += step;
    }
    return out;
}

void update_traffic(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("traffic.update");
    auto& w = scene.world();
    auto* twp = w.resource<TrafficWorld>();
    if (!twp || !twp->graph) return;
    TrafficWorld& tw = *twp;
    const LaneGraph& g = *tw.graph;
    const auto t0 = std::chrono::steady_clock::now();
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    tw.time += static_cast<double>(dt);
    TrafficStats stats;

    std::vector<Agent> agents;
    w.each<TrafficAgentComponent>([&](Entity e, TrafficAgentComponent& c) {
        if (!c.enabled) return;
        const std::uint32_t seed = c.seed ? c.seed : e.index + 1u;
        if (c.lane >= g.lanes.size()) {
            const auto* tr = w.get<scene::Transform>(e);
            float s = 0;
            c.lane = g.nearest_lane(tr ? tr->position : Vec3{}, &s);
            if (c.lane == kNone) return;
            c.distance = s + 0.5f * c.length;
            c.speed = 0;
            c.next = kNone;
        }
        c.distance = std::clamp(c.distance, 0.0f, g.lanes[c.lane].length);
        if (c.next == kNone || std::find(g.lanes[c.lane].next.begin(), g.lanes[c.lane].next.end(), c.next) == g.lanes[c.lane].next.end())
            c.next = choose_next(g, c.lane, seed, c.turns);
        agents.push_back({e, &c, seed});
    });
    stats.agents = static_cast<std::uint32_t>(agents.size());

    // Per-lane occupancy sorted by distance (snapshot at frame start).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> on_lane;
    on_lane.reserve(agents.size());
    for (std::uint32_t i = 0; i < agents.size(); ++i) on_lane[agents[i].c->lane].push_back(i);
    for (auto& [lane, list] : on_lane)
        std::sort(list.begin(), list.end(), [&](std::uint32_t x, std::uint32_t y) {
            const float dx = agents[x].c->distance, dy = agents[y].c->distance;
            return dx != dy ? dx < dy : agents[x].entity.index < agents[y].entity.index;
        });
    struct Snap {
        float distance, speed, length;
        std::uint32_t lane, next;
    };
    std::vector<Snap> snap(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        const auto& c = *agents[i].c;
        snap[i] = {c.distance, c.speed, c.length, c.lane, c.next};
    }
    auto first_on = [&](std::uint32_t lane) -> std::int64_t {
        auto it = on_lane.find(lane);
        return it == on_lane.end() || it->second.empty() ? -1 : static_cast<std::int64_t>(it->second.front());
    };

    auto find_leader = [&](std::uint32_t self) {
        const Snap& me = snap[self];
        const Lane& lane = g.lanes[me.lane];
        Leader best;
        auto consider = [&](float gap, float v, bool light) {
            if (gap < best.gap) best = {gap, v, light};
        };
        // Same lane, ahead.
        const auto& list = on_lane[me.lane];
        const auto pos = std::find(list.begin(), list.end(), self);
        if (pos != list.end() && pos + 1 != list.end()) {
            const Snap& o = snap[*(pos + 1)];
            consider(o.distance - o.length - me.distance, o.speed, false);
        }
        const float rem = lane.length - me.distance;
        const float horizon = std::max(60.0f, me.speed * me.speed / 2.0f + 30.0f);
        if (me.next == kNone) {
            consider(rem, 0, false); // dead end without a way on
            return best;
        }
        // Light at the end of a driving lane, for the connector we chose.
        const Lane& nl = g.lanes[me.next];
        if (!lane.connector && nl.connector) {
            const Light light = g.light(me.next, tw.time);
            if (light == Light::Red) consider(rem, 0, true);
            else if (light == Light::Amber) {
                const float b = agents[self].c->deceleration;
                if (me.speed * me.speed / (2.0f * b) < rem + 0.5f) consider(rem, 0, true);
            }
        }
        // Vehicles ahead on the following lanes.
        float ahead = rem;
        std::uint32_t l = me.next;
        while (l != kNone && ahead < horizon) {
            if (const std::int64_t f = first_on(l); f >= 0) {
                const Snap& o = snap[static_cast<std::size_t>(f)];
                consider(ahead + o.distance - o.length, o.speed, false);
                break;
            }
            ahead += g.lanes[l].length;
            l = g.lanes[l].next.size() == 1 ? g.lanes[l].next[0] : kNone;
        }
        // Merges: other lanes feeding the same next lane; whoever is nearer the merge goes first.
        const std::uint32_t merge = lane.connector ? (lane.next.empty() ? kNone : lane.next[0]) : (nl.connector && !nl.next.empty() ? nl.next[0] : kNone);
        if (merge != kNone && g.lanes[merge].prev.size() > 1) {
            const float my_to_merge = lane.connector ? rem : rem + nl.length;
            for (const std::uint32_t feeder : g.lanes[merge].prev) {
                if (feeder == me.lane || feeder == me.next) continue;
                const auto it = on_lane.find(feeder);
                if (it == on_lane.end() || it->second.empty()) continue;
                // The agent on the feeder connector nearest its end.
                const std::uint32_t o_idx = it->second.back();
                const Snap& o = snap[o_idx];
                const float o_to_merge = g.lanes[feeder].length - o.distance;
                const bool o_first = o_to_merge < my_to_merge ||
                                     (o_to_merge == my_to_merge && agents[o_idx].entity.index < agents[self].entity.index);
                if (o_first) consider(my_to_merge - o_to_merge - o.length, o.speed, false);
            }
        }
        return best;
    };

    for (std::uint32_t i = 0; i < agents.size(); ++i) {
        Agent& a = agents[i];
        TrafficAgentComponent& c = *a.c;
        const gameplay::LodTick tick = gameplay::simulation_lod_tick(w.get<gameplay::SimulationLodComponent>(a.entity), dt);
        if (!tick.due) {
            stats.mean_speed += c.speed;
            if (c.speed < 0.5f) ++stats.stopped;
            continue;
        }
        ++stats.updated;
        Leader lead = find_leader(i);
        const float a_max = std::max(c.acceleration, 0.1f), b = std::max(c.deceleration, 0.1f);
        float remaining_dt = std::max(tick.dt, 0.0f);
        while (remaining_dt > 1e-6f) {
            const float h = std::min(remaining_dt, kMaxSubstep);
            remaining_dt -= h;
            const Lane& lane = g.lanes[c.lane];
            float v0 = std::min(std::max(c.max_speed, 0.1f), lane.speed_limit);
            const float rem = lane.length - c.distance;
            if (c.next != kNone) {
                const float vn = std::min(std::max(c.max_speed, 0.1f), g.lanes[c.next].speed_limit);
                if (vn < v0) v0 = std::max(std::min(v0, std::sqrt(vn * vn + 2.0f * b * std::max(rem, 0.0f))), 0.1f);
            }
            const float v = c.speed;
            float acc = a_max * (1.0f - std::pow(v / v0, 4.0f));
            if (std::isfinite(lead.gap)) {
                const float s_star = std::max(c.min_gap, 0.0f) +
                                     std::max(0.0f, v * c.time_headway + v * (v - lead.speed) / (2.0f * std::sqrt(a_max * b)));
                const float gap = std::max(lead.gap - (lead.light ? 0.5f : 0.0f), 0.1f);
                acc -= a_max * (s_star / gap) * (s_star / gap);
            }
            acc = std::max(acc, -9.0f);
            float nv = v + acc * h;
            float ds;
            if (nv < 0) {
                ds = acc < 0 ? -0.5f * v * v / acc : 0.0f;
                nv = 0;
            } else {
                ds = 0.5f * (v + nv) * h;
            }
            c.speed = nv;
            c.distance += ds;
            if (std::isfinite(lead.gap)) lead.gap += lead.speed * h - ds;
            while (c.distance >= g.lanes[c.lane].length) {
                if (c.next == kNone) {
                    c.distance = g.lanes[c.lane].length;
                    c.speed = 0;
                    break;
                }
                c.distance -= g.lanes[c.lane].length;
                c.lane = c.next;
                ++c.turns;
                ++stats.lane_changes;
                c.next = choose_next(g, c.lane, a.seed, c.turns);
            }
        }
        if (lead.light && c.speed < 0.5f) ++stats.waiting_at_light;
        if (c.speed < 0.5f) ++stats.stopped;
        stats.mean_speed += c.speed;
        if (auto* tr = w.get<scene::Transform>(a.entity)) {
            Vec3 t;
            tr->position = g.point_at(c.lane, c.distance - 0.5f * c.length, &t);
            const float pitch = -std::atan2(t.y, std::max(math::length(flat(t)), 1e-6f));
            tr->rotation = math::Quat::euler(pitch, std::atan2(t.x, t.z), 0);
        }
    }
    if (stats.agents) stats.mean_speed /= static_cast<float>(stats.agents);
    stats.update_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    tw.stats = stats;
}

NetworkLoader directory_loader(std::filesystem::path assets_root) {
    return [root = std::move(assets_root)](std::string_view path) -> Result<std::string> {
        auto clean = fs::sanitize_relative(path);
        if (!clean) return std::move(clean).error();
        return fs::read_text(root / *clean, 64u * 1024u * 1024u);
    };
}

bool build_authored_network(scene::Scene& scene, const NetworkLoader& loader) {
    auto& w = scene.world();
    Entity owner{};
    TrafficNetworkComponent* net_c = nullptr;
    w.each<TrafficNetworkComponent>([&](Entity e, TrafficNetworkComponent& c) {
        if (!net_c && !c.built) {
            owner = e;
            net_c = &c;
        }
    });
    if (!net_c) return true;
    net_c->built = true;
    RoadNetwork net;
    if (!net_c->network.empty()) {
        if (!loader) {
            VICTUS_LOG_WARN("traffic", "no loader for network '{}'", net_c->network);
            return false;
        }
        auto text = loader(net_c->network);
        if (!text) {
            VICTUS_LOG_WARN("traffic", "cannot read '{}': {}", net_c->network, text.error().message);
            return false;
        }
        auto parsed = parse_road_network(std::string_view(*text));
        if (!parsed) {
            VICTUS_LOG_WARN("traffic", "'{}': {}", net_c->network, parsed.error().message);
            return false;
        }
        net = std::move(parsed).value();
    } else {
        // The graph lives in the owner's local space: agents are its children.
        net = grid_network(std::clamp<std::uint32_t>(net_c->columns, 1, 64), std::clamp<std::uint32_t>(net_c->rows, 1, 64),
                           std::max(net_c->block, 10.0f), std::clamp<std::uint32_t>(net_c->lanes, 1, 4), Vec3{},
                           std::max(net_c->speed_limit, 1.0f));
    }
    LaneGraphSettings settings;
    settings.left_hand = net_c->left_hand;
    settings.green = std::max(net_c->green, 1.0f);
    const std::uint32_t vehicles = net_c->vehicles, seed = net_c->seed;
    set_traffic_graph(scene, std::make_shared<LaneGraph>(build_lane_graph(net, settings)));
    // Copy the template before spawning so the agents become its siblings' children (not its).
    const std::vector<Entity> kids = scene.children(owner);
    const Entity templ = kids.empty() ? Entity{} : kids.front();
    const std::vector<Entity> cars = spawn_traffic(scene, vehicles, seed, owner);
    if (templ != Entity{}) {
        for (const Entity car : cars) {
            const Entity copy = scene.duplicate(templ);
            scene.set_parent(copy, car);
            if (auto* tr = w.get<scene::Transform>(copy)) *tr = scene::Transform{};
        }
        // Hide the template's meshes (copies were made above with their own visibility).
        std::vector<Entity> stack{templ};
        while (!stack.empty()) {
            const Entity e = stack.back();
            stack.pop_back();
            if (auto* mr = w.get<scene::MeshRendererComponent>(e)) mr->visible = false;
            for (const Entity k : scene.children(e)) stack.push_back(k);
        }
    }
    return true;
}

void add_traffic_systems(gameplay::Simulation& sim, NetworkLoader loader) {
    register_components();
    sim.add_system("traffic.update", gameplay::Phase::Update, [loader = std::move(loader)](scene::Scene& s, const gameplay::FrameTime& t) {
        if (gameplay::game_paused(s)) return;
        if (s.world().count<TrafficNetworkComponent>() > 0) (void)build_authored_network(s, loader);
        update_traffic(s, t.dt);
    });
}

} // namespace victus::traffic

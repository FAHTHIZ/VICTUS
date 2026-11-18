/* SPDX-License-Identifier: Apache-2.0 */
/* Test plugin built in several modes (TEST_PLUGIN_MODE) for tests/plugin/plugin_tests.cpp. */
#include "victus/plugin/victus_plugin.h"

#include <stdio.h>
#include <string.h>

#define MODE_OK 0
#define MODE_FAIL_INIT 1
#define MODE_NEWER_API 2
#define MODE_NO_ENTRY 3

static const VictusHostApi* g_host;
static const VictusLogV1* g_log;
static const VictusSystemsV1* g_systems;
static const VictusSceneV1* g_scene;
static const VictusFunctionsV1* g_functions;
static int g_ticks;

static void mover(void* user, VictusScene* scene, float dt) {
    (void)user;
    VictusEntity e = g_scene->find_by_name(scene, "Mover");
    float p[3];
    if (e == VICTUS_NULL_ENTITY || g_scene->get_position(scene, e, p) != VICTUS_PLUGIN_OK) return;
    p[0] += dt;
    g_scene->set_position(scene, e, p);
    ++g_ticks;
    if (g_ticks == 1) {
        VictusEntity child = g_scene->create(scene, "PluginChild", e);
        float s[3] = {2.0f, 2.0f, 2.0f};
        g_scene->set_scale(scene, child, s);
    }
}

static int greet(void* user, const char* args, VictusCallResult* result) {
    (void)user;
    (void)args;
    g_functions->set_result(result, "\"hello\"");
    return VICTUS_PLUGIN_OK;
}
static int echo(void* user, const char* args, VictusCallResult* result) {
    (void)user;
    g_functions->set_result(result, args);
    return VICTUS_PLUGIN_OK;
}
static int ticks(void* user, const char* args, VictusCallResult* result) {
    char buf[32];
    (void)user;
    (void)args;
    snprintf(buf, sizeof buf, "%d", g_ticks);
    g_functions->set_result(result, buf);
    return VICTUS_PLUGIN_OK;
}
static void nop_system(void* user, VictusScene* scene, float dt) {
    (void)user;
    (void)scene;
    (void)dt;
}
/* Returns the add_system result when called after init (must be an error). */
static int late_add(void* user, const char* args, VictusCallResult* result) {
    char buf[16];
    (void)user;
    (void)args;
    snprintf(buf, sizeof buf, "%d", g_systems->add_system(g_host->host, "late", VICTUS_PHASE_UPDATE, nop_system, NULL));
    g_functions->set_result(result, buf);
    return VICTUS_PLUGIN_OK;
}
static int fail(void* user, const char* args, VictusCallResult* result) {
    (void)user;
    (void)args;
    g_functions->set_error(result, "nope");
    return VICTUS_PLUGIN_ERROR;
}
/* args: a JSON string naming another plugin function; returns that function's result. */
static int relay(void* user, const char* args, VictusCallResult* result) {
    char name[128];
    char out[256];
    size_t len = strlen(args), n = 0;
    (void)user;
    if (len < 2 || args[0] != '"' || len - 2 >= sizeof name) {
        g_functions->set_error(result, "expected a string");
        return VICTUS_PLUGIN_ERROR;
    }
    memcpy(name, args + 1, len - 2);
    name[len - 2] = '\0';
    if (g_functions->call(g_host->host, name, "null", out, sizeof out, &n) != VICTUS_PLUGIN_OK) {
        g_functions->set_error(result, "relay target failed");
        return VICTUS_PLUGIN_ERROR;
    }
    g_functions->set_result(result, out);
    return VICTUS_PLUGIN_OK;
}

static int plugin_init(void* user) {
    (void)user;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "init");
    if (g_systems->add_system(g_host->host, "mover", VICTUS_PHASE_UPDATE, mover, NULL) != VICTUS_PLUGIN_OK) return 1;
    if (g_systems->add_system(g_host->host, "bad", 99, mover, NULL) == VICTUS_PLUGIN_OK) return 1;
    g_functions->register_function(g_host->host, "greet", greet, NULL);
    g_functions->register_function(g_host->host, "echo", echo, NULL);
    g_functions->register_function(g_host->host, "ticks", ticks, NULL);
    g_functions->register_function(g_host->host, "late_add", late_add, NULL);
    g_functions->register_function(g_host->host, "fail", fail, NULL);
    g_functions->register_function(g_host->host, "relay", relay, NULL);
    if (g_functions->register_function(g_host->host, "greet", greet, NULL) == VICTUS_PLUGIN_OK) return 1;
#if TEST_PLUGIN_MODE == MODE_FAIL_INIT
    g_host->set_error(g_host->host, "boom");
    return VICTUS_PLUGIN_ERROR;
#else
    return VICTUS_PLUGIN_OK;
#endif
}
static void plugin_shutdown(void* user) {
    (void)user;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "shutdown");
}

#if TEST_PLUGIN_MODE == MODE_NO_ENTRY
VICTUS_PLUGIN_EXPORT int some_other_symbol(void) { return 0; }
#else
VICTUS_PLUGIN_EXPORT int victus_plugin_entry(const VictusHostApi* host, VictusPluginDesc* out) {
    if (host->api_major != VICTUS_PLUGIN_API_MAJOR) return VICTUS_PLUGIN_ERROR;
    g_host = host;
    g_log = (const VictusLogV1*)host->get_interface(host->host, VICTUS_LOG_INTERFACE_1);
    g_systems = (const VictusSystemsV1*)host->get_interface(host->host, VICTUS_SYSTEMS_INTERFACE_1);
    g_scene = (const VictusSceneV1*)host->get_interface(host->host, VICTUS_SCENE_INTERFACE_1);
    g_functions = (const VictusFunctionsV1*)host->get_interface(host->host, VICTUS_FUNCTIONS_INTERFACE_1);
    if (!g_log || !g_systems || !g_scene || !g_functions) return VICTUS_PLUGIN_ERROR;
    if (host->get_interface(host->host, "victus.does_not_exist.1") != NULL) return VICTUS_PLUGIN_ERROR;
    out->struct_size = sizeof *out;
    out->api_major = VICTUS_PLUGIN_API_MAJOR;
#if TEST_PLUGIN_MODE == MODE_NEWER_API
    out->api_minor = VICTUS_PLUGIN_API_MINOR + 1;
#else
    out->api_minor = VICTUS_PLUGIN_API_MINOR;
#endif
    out->user = NULL;
    out->init = plugin_init;
    out->shutdown = plugin_shutdown;
    return VICTUS_PLUGIN_OK;
}
#endif

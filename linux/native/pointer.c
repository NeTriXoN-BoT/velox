/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "pointer-constraints.h"
#include "relative-pointer.h"

#define VELOX_EXPORT __attribute__((visibility("default")))

struct velox_pointer {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_pointer *pointer;
    struct wl_surface *surface;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_callback *ready;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_relative_pointer_v1 *relative;
    struct zwp_locked_pointer_v1 *locked_pointer;
    uint32_t constraints_name, relative_name;
    int wanted, locked, initialized;
    int x, y, width, height;
    double dx, dy;
};

static void apply(struct velox_pointer *state);

static void locked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    (void)pointer;
    struct velox_pointer *state = data;
    state->locked = 1;
    state->dx = state->dy = 0;
}

static void unlocked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    (void)pointer;
    struct velox_pointer *state = data;
    state->wanted = state->locked = 0;
    state->dx = state->dy = 0;
}

static const struct zwp_locked_pointer_v1_listener lock_events = {locked, unlocked};

static void motion(void *data, struct zwp_relative_pointer_v1 *pointer, uint32_t high, uint32_t low,
                   wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t raw_dx, wl_fixed_t raw_dy)
{
    (void)pointer; (void)high; (void)low; (void)dx; (void)dy;
    struct velox_pointer *state = data;
    if (!state->locked)
        return;
    state->dx = fmax(-32768., fmin(32768., state->dx + wl_fixed_to_double(raw_dx)));
    state->dy = fmax(-32768., fmin(32768., state->dy + wl_fixed_to_double(raw_dy)));
}

static const struct zwp_relative_pointer_v1_listener relative_events = {motion};

static struct wl_region *region(struct velox_pointer *state)
{
    struct wl_region *value = wl_compositor_create_region(state->compositor);
    if (value)
        wl_region_add(value, state->x, state->y, state->width, state->height);
    return value;
}

static void apply(struct velox_pointer *state)
{
    if (!state->wanted || state->locked_pointer || !state->constraints || !state->relative_manager)
        return;
    if (!state->relative) {
        state->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(state->relative_manager, state->pointer);
        if (!state->relative)
            return;
        zwp_relative_pointer_v1_add_listener(state->relative, &relative_events, state);
    }
    struct wl_region *area = region(state);
    if (!area)
        return;
    state->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(state->constraints, state->surface, state->pointer,
        area, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_region_destroy(area);
    if (state->locked_pointer)
        zwp_locked_pointer_v1_add_listener(state->locked_pointer, &lock_events, state);
}

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)version;
    struct velox_pointer *state = data;
    if (!strcmp(interface, "zwp_pointer_constraints_v1") && !state->constraints) {
        state->constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
        state->constraints_name = name;
    } else if (!strcmp(interface, "zwp_relative_pointer_manager_v1") && !state->relative_manager) {
        state->relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
        state->relative_name = name;
    }
    apply(state);
}

static void remove_global(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct velox_pointer *state = data;
    if (name == state->constraints_name || name == state->relative_name)
        state->wanted = state->locked = 0;
}

static const struct wl_registry_listener registry_events = {global, remove_global};

static void initialized(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial;
    struct velox_pointer *state = data;
    state->initialized = 1;
    wl_callback_destroy(callback);
    state->ready = NULL;
}

static const struct wl_callback_listener ready_events = {initialized};

VELOX_EXPORT int velox_pointer_abi(void) { return 1; }

VELOX_EXPORT void velox_pointer_destroy(struct velox_pointer *state)
{
    if (!state)
        return;
    if (state->locked_pointer) zwp_locked_pointer_v1_destroy(state->locked_pointer);
    if (state->relative) zwp_relative_pointer_v1_destroy(state->relative);
    if (state->constraints) zwp_pointer_constraints_v1_destroy(state->constraints);
    if (state->relative_manager) zwp_relative_pointer_manager_v1_destroy(state->relative_manager);
    if (state->ready) wl_callback_destroy(state->ready);
    if (state->registry) wl_registry_destroy(state->registry);
    if (state->display) wl_display_flush(state->display);
    if (state->queue) wl_event_queue_destroy(state->queue);
    free(state);
}

VELOX_EXPORT struct velox_pointer *velox_pointer_create(void *display, void *compositor, void *pointer, void *surface)
{
    if (!display || !compositor || !pointer || !surface || strcmp(wl_proxy_get_class(surface), "wl_surface") ||
        strcmp(wl_proxy_get_class(pointer), "wl_pointer") || strcmp(wl_proxy_get_class(compositor), "wl_compositor"))
        return NULL;
    struct velox_pointer *state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;
    state->display = display; state->compositor = compositor; state->pointer = pointer; state->surface = surface;
    state->width = state->height = 1;
    state->queue = wl_display_create_queue(display);
    if (!state->queue) {
        free(state);
        return NULL;
    }
    /* A wrapper sets the queue only for our new proxies. Qt keeps its own
       display, pointer, compositor and surface queues and their ownership. */
    void *wrapper = wl_proxy_create_wrapper(display);
    if (!wrapper) {
        velox_pointer_destroy(state);
        return NULL;
    }
    wl_proxy_set_queue(wrapper, state->queue);
    state->registry = wl_display_get_registry(wrapper);
    state->ready = wl_display_sync(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (!state->registry || !state->ready) {
        velox_pointer_destroy(state);
        return NULL;
    }
    wl_registry_add_listener(state->registry, &registry_events, state);
    wl_callback_add_listener(state->ready, &ready_events, state);
    wl_display_flush(display);
    return state;
}

VELOX_EXPORT void velox_pointer_region(struct velox_pointer *state, int x, int y, int width, int height)
{
    if (!state || width < 1 || width > 16384 || height < 1 || height > 16384 || x < 0 || y < 0)
        return;
    state->x = x; state->y = y; state->width = width; state->height = height;
    if (state->locked_pointer) {
        struct wl_region *area = region(state);
        if (area) {
            zwp_locked_pointer_v1_set_region(state->locked_pointer, area);
            wl_region_destroy(area);
        }
    }
}

VELOX_EXPORT void velox_pointer_request(struct velox_pointer *state, int wanted)
{
    if (!state)
        return;
    state->wanted = !!wanted;
    state->dx = state->dy = 0;
    if (!wanted && state->locked_pointer) {
        zwp_locked_pointer_v1_destroy(state->locked_pointer);
        state->locked_pointer = NULL;
        state->locked = 0;
    }
    apply(state);
    wl_display_flush(state->display);
}

VELOX_EXPORT int velox_pointer_poll(struct velox_pointer *state, double *dx, double *dy)
{
    if (!state || !dx || !dy || wl_display_dispatch_queue_pending(state->display, state->queue) < 0)
        return -1;
    *dx = state->dx; *dy = state->dy;
    state->dx = state->dy = 0;
    wl_display_flush(state->display);
    if (state->initialized && (!state->constraints || !state->relative_manager))
        return 2;
    return state->locked ? 1 : 0;
}

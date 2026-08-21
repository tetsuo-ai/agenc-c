/* main.c: a printable walkthrough of the arena public API. */

#include <stdio.h>
#include <string.h>

#include "alloc.h"
#include "arena.h"

struct point {
    double x;
    double y;
};

static void demo_growing(void)
{
    arena_t a;
    arena_temp_t temp;
    char *name;
    struct point *points;
    size_t idx;

    printf("== growing arena ==\n");
    arena_init(&a, alloc_libc());

    name = arena_memdup(&a, "AgenC", 6);
    points = ARENA_NEW_N(&a, struct point, 4);
    if (arena_failed(&a)) {
        printf("allocation failed: %s\n", arena_status_name(arena_status(&a)));
        arena_deinit(&a);
        return;
    }
    for (idx = 0; idx < 4; idx++) {
        points[idx].x = (double)idx;
        points[idx].y = (double)idx * 2.0;
    }
    printf("name=%s points[3]=(%g, %g)\n", name, points[3].x, points[3].y);
    printf("used=%zu committed=%zu high_water=%zu\n", arena_used(&a), arena_committed(&a),
           arena_high_water(&a));

    /* Scratch work inside a temp scope costs nothing after it ends. */
    temp = arena_temp_begin(&a);
    for (idx = 0; idx < 100; idx++) {
        (void)arena_alloc(&a, 512);
    }
    printf("inside temp: used=%zu\n", arena_used(&a));
    arena_temp_end(temp);
    printf("after temp:  used=%zu (blocks retained: committed=%zu)\n", arena_used(&a),
           arena_committed(&a));

    /* Reset keeps the memory for reuse; deinit returns it. */
    arena_reset(&a);
    printf("after reset: used=%zu committed=%zu\n", arena_used(&a), arena_committed(&a));
    arena_deinit(&a);
}

static void demo_fixed(void)
{
    static _Alignas(max_align_t) unsigned char buffer[1024];
    arena_t a;
    char *text;

    printf("== fixed arena (no heap) ==\n");
    arena_init_fixed(&a, buffer, sizeof(buffer));
    text = arena_memdup(&a, "heap-free", 10);
    printf("text=%s used=%zu of %zu\n", text, arena_used(&a), arena_committed(&a));

    /* Exhaustion is a clean, sticky failure. */
    if (arena_alloc(&a, sizeof(buffer)) == NULL) {
        printf("exhaustion: %s (sticky until cleared)\n", arena_status_name(arena_status(&a)));
    }
    arena_clear_error(&a);
    printf("recovered: ok=%d\n", arena_ok(&a));
    arena_deinit(&a);
}

static void demo_adapter(void)
{
    arena_t a;
    alloc_t ad;
    void *ptr;

    printf("== arena as an allocator ==\n");
    arena_init(&a, alloc_libc());
    ad = arena_allocator(&a);

    /* LIFO alloc/free pairs cost zero net memory through the vtable. */
    ptr = alloc_alloc(&ad, 128, 0);
    printf("used after alloc: %zu\n", arena_used(&a));
    alloc_free(&ad, ptr, 128, 0);
    printf("used after free:  %zu\n", arena_used(&a));

    arena_deinit(&a);
}

int main(void)
{
    demo_growing();
    demo_fixed();
    demo_adapter();
    return 0;
}

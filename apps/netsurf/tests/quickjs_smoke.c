/* Host-side QuickJS-ng smoke test for the proposed PAX integration.
 * Build against QuickJS-ng v0.17.0 with atomics disabled. */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"

static int interrupts;
static int interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    return ++interrupts > 5;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return 1;
    JS_SetMemoryLimit(rt, 4u * 1024u * 1024u);
    JS_SetMaxStackSize(rt, 128u * 1024u);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 2; }
    const char *basic = "let n = 40; n + 2";
    JSValue value = JS_Eval(ctx, basic, strlen(basic), "basic.js", JS_EVAL_TYPE_GLOBAL);
    int32_t result = 0;
    int failed = JS_IsException(value) || JS_ToInt32(ctx, &result, value) != 0 || result != 42;
    JS_FreeValue(ctx, value);
    const char *too_large = "new ArrayBuffer(10 * 1024 * 1024)";
    value = JS_Eval(ctx, too_large, strlen(too_large), "memory.js", JS_EVAL_TYPE_GLOBAL);
    failed |= !JS_IsException(value);
    JS_FreeValue(ctx, value);
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
    JS_SetInterruptHandler(rt, interrupt, NULL);
    const char *loop = "for (;;) {}";
    value = JS_Eval(ctx, loop, strlen(loop), "loop.js", JS_EVAL_TYPE_GLOBAL);
    failed |= !JS_IsException(value) || interrupts <= 5;
    JS_FreeValue(ctx, value);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts(failed ? "QuickJS smoke test failed" : "QuickJS evaluation, memory limit, and interruption passed");
    return failed ? 1 : 0;
}

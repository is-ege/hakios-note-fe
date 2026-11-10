/* Native keyboard dimensions at their providers, before view creation/layout.
 * All originals are captured before installing any override, including inherited
 * methods. Frame/bounds/center setters remain UIKit's own implementations.
 */
struct KeyboardSizeHook { Class owner; SEL selector; IMP original; unsigned arity; BOOL class_method; };
static struct KeyboardSizeHook keyboard_size_hooks[16];
static unsigned keyboard_size_hook_count, keyboard_size_calls, emoji_invalidation_calls;
static BOOL (*original_emoji_invalidation)(id, SEL, id, id);
static void (*original_keyplane_name)(id, SEL, id);
static id (*original_keyboard_hit)(id, SEL, CGPoint);
static unsigned keyplane_trace_count, key_hit_trace_count;

/* Opt-in observation of layout transitions, never entered text. */
static void
trace_keyplane_name(id self, SEL selector, id name)
{
    const char *label = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : "nil";
    BOOL traced = __atomic_fetch_add(&keyplane_trace_count, 1, __ATOMIC_RELAXED) < 32;
    static _Thread_local unsigned depth;
    if (traced) say("kbtrace: %s: keyplane to set: %.120s\n", getprogname(), label ? label : "?");
    if (depth > 0 && traced) {          /* set while another is being set: by whom */
        void *frames[24];
        int count = backtrace(frames, 24);
        /* the cache's own names are stripped: the nearest method below each address, of UIKit's classes */
        const char *owner[24] = {0}, *method[24] = {0};
        uintptr_t best[24] = {0};
        char side[24] = {0};
        unsigned classes = 0;
        Class *list = objc_copyClassList(&classes);
        for (unsigned c = 0; c < classes; c++) {
            const char *name = class_getName(list[c]);
            if (strncmp(name, "UI", 2) != 0 && strncmp(name, "_UI", 3) != 0) continue;
            for (unsigned meta = 0; meta < 2; meta++) {
                unsigned methods = 0;
                Method *all = class_copyMethodList(meta ? object_getClass(list[c]) : list[c], &methods);
                for (unsigned m = 0; m < methods; m++) {
                    uintptr_t at = (uintptr_t)method_getImplementation(all[m]) & 0x0000007fffffffffUL;
                    for (int f = 1; f < count; f++) {
                        uintptr_t frame = (uintptr_t)frames[f] & 0x0000007fffffffffUL;
                        if (at <= frame && at > best[f]) {
                            best[f] = at; owner[f] = name; method[f] = sel_getName(method_getName(all[m])); side[f] = meta ? '+' : '-';
                        }
                    }
                }
                free(all);
            }
        }
        free(list);
        for (int f = 1; f < count; f++)
            say("kbtrace: %s:   %d %c[%s %s] + %lu\n", getprogname(), f, side[f] ? side[f] : '?', owner[f] ? owner[f] : "?",
                method[f] ? method[f] : "?", (unsigned long)(((uintptr_t)frames[f] & 0x0000007fffffffffUL) - best[f]));
    }
    depth++;
    original_keyplane_name(self, selector, name);
    depth--;
    if (traced) say("kbtrace: %s: keyplane set %.120s\n", getprogname(), label ? label : "?");
}

/* a key as the layout names it, and how it is to be handled */
static void
trace_key(const char *what, CGPoint point, id key)
{
    SEL name = sel_registerName("name"), kind = sel_registerName("interactionType"), state = sel_registerName("state");
    id text = key && class_respondsToSelector(object_getClass(key), name) ? ((id (*)(id, SEL))objc_msgSend)(key, name) : nil;
    const char *label = text ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "nil";
    say("kbtrace: %s: %s %.1f,%.1f key %.80s interaction %d state %d\n", getprogname(), what, point.x, point.y,
        label ? label : "?",
        key && class_respondsToSelector(object_getClass(key), kind) ? ((int (*)(id, SEL))objc_msgSend)(key, kind) : -1,
        key && class_respondsToSelector(object_getClass(key), state) ? ((int (*)(id, SEL))objc_msgSend)(key, state) : -1);
}

static id
trace_keyboard_hit(id self, SEL selector, CGPoint point)
{
    id key = original_keyboard_hit(self, selector, point);
    if (__atomic_load_n(&key_hit_trace_count, __ATOMIC_RELAXED) < 48) trace_key("hit", point, key);
    if (point.x < 120 && point.y > 100 &&
        __atomic_fetch_add(&key_hit_trace_count, 1, __ATOMIC_RELAXED) < 48) {
        CGRect frame = {{0, 0}, {0, 0}};
        Method getter = key ? class_getInstanceMethod(object_getClass(key), sel_registerName("frame")) : NULL;
        if (getter && strcmp(method_getTypeEncoding(getter), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8") == 0)
            frame = ((CGRect (*)(id, SEL))objc_msgSend)(key, sel_registerName("frame"));
        say("kbtrace: %s: lower-left hit %.6f,%.6f key %s frame %.6f,%.6f %.6fx%.6f\n",
            getprogname(), point.x, point.y, key ? object_getClassName(key) : "nil",
            frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
    }
    return key;
}

/* a touch as the layout is given it: its own time against the clock's now */
static void (*original_touch[2])(id, SEL, id, id);
static unsigned touch_trace_count;

static void
trace_touch(unsigned which, id self, SEL selector, id touch, id context)
{
    if (__atomic_fetch_add(&touch_trace_count, 1, __ATOMIC_RELAXED) < 40) {
        SEL stamp = sel_registerName("timestamp");
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        double now = (double)mach_absolute_time() * base.numer / base.denom / 1e9;
        double at = touch && class_respondsToSelector(object_getClass(touch), stamp) ?
            ((double (*)(id, SEL))objc_msgSend)(touch, stamp) : -1;
        say("kbtrace: %s: touch %s: %s timestamp %.4f, now %.4f (%.1f ms ago); timebase %u/%u\n", getprogname(),
            which ? "up" : "down", touch ? object_getClassName(touch) : "nil", at, now, (now - at) * 1000, base.numer, base.denom);
    }
    original_touch[which](self, selector, touch, context);
}
static void trace_touch_down(id self, SEL selector, id touch, id context) { trace_touch(0, self, selector, touch, context); }
static void trace_touch_up(id self, SEL selector, id touch, id context) { trace_touch(1, self, selector, touch, context); }

static void
install_native_keyboard_trace(void)
{
    /* (a program other than SpringBoard may not have UIKit in it yet when a library of ours starts) */
    dlopen("/System/Library/PrivateFrameworks/UIKitCore.framework/UIKitCore", RTLD_NOW);
    Class cls = objc_getClass("UIKeyboardLayoutStar");
    unsigned count = 0;
    Method *methods = cls ? class_copyMethodList(cls, &count) : NULL;
    for (unsigned i = 0; i < count; i++) {
        const char *selector = sel_getName(method_getName(methods[i]));
        if (strcasestr(selector, "touchDown") || strcasestr(selector, "ignore") || strcasestr(selector, "moreKey") ||
            strcasestr(selector, "shouldAllow") || strcasestr(selector, "keyplaneFor") || strcasestr(selector, "shouldHit"))
            say("kbtrace: %s: method %s %s\n", getprogname(), selector, method_getTypeEncoding(methods[i]));
    }
    free(methods);
    Class layout = objc_getClass("UIKeyboardLayout");
    Method small = layout ? class_getClassMethod(layout, sel_registerName("_showSmallDisplayKeyplane")) : NULL;
    if (small) {
        const uint32_t *code = (const uint32_t *)method_getImplementation(small);
        say("kbtrace: %s: _showSmallDisplayKeyplane answers %d; its code at %p:\n", getprogname(),
            ((BOOL (*)(id, SEL))objc_msgSend)((id)layout, sel_registerName("_showSmallDisplayKeyplane")), (void *)code);
        /* its third instruction is a call: to what */
        const uint32_t *target = code + 2 + (((int32_t)(code[2] << 6)) >> 6);
        Dl_info info = {0};
        dladdr(target, &info);
        say("kbtrace: %s: it calls %p: %s in %s; code there %08x %08x %08x %08x\n", getprogname(), (void *)target,
            info.dli_sname ? info.dli_sname : "?", info.dli_fname ? info.dli_fname : "?", target[0], target[1], target[2],
            target[3]);
        if ((target[0] & 0x9f000000) == 0x90000000 && (target[1] & 0xffc00000) == 0x91000000) {       /* adrp, add, br: an island */
            uint64_t pages = ((uint64_t)(target[0] >> 5) & 0x7ffff) << 2 | ((target[0] >> 29) & 3);
            const uint32_t *real = (const uint32_t *)(((uintptr_t)target & ~0xfffUL) + ((intptr_t)((int64_t)(pages << 43) >> 43) << 12) +
                ((target[1] >> 10) & 0xfff));
            dladdr(real, &info);
            say("kbtrace: %s: ... an island to %p: %s in %s\n", getprogname(), (void *)real, info.dli_sname ? info.dli_sname : "?",
                info.dli_fname ? info.dli_fname : "?");
        }
    }
    const char *owners[] = {"UIKeyboardLayoutStar", "UIKBScreenTraits", "UIKeyboardImpl", "UIKBTree", "UIKeyboardLayout",
                            "UIKeyboardCache", "UIKBKeyplaneView"};
    for (unsigned o = 0; o < sizeof(owners) / sizeof(owners[0]); o++)
        for (unsigned side = 0; side < 2; side++) {
            Class owner = objc_getClass(owners[o]);
            methods = owner ? class_copyMethodList(side ? object_getClass(owner) : owner, &count) : NULL;
            for (unsigned i = 0; methods && i < count; i++) {
                const char *selector = sel_getName(method_getName(methods[i]));
                if (strcasestr(selector, "small") || strcasestr(selector, "variant") || strcasestr(selector, "suffix"))
                    say("kbtrace: %s: %c[%s %s] %s\n", getprogname(), side ? '+' : '-', owners[o], selector,
                        method_getTypeEncoding(methods[i]));
            }
            free(methods);
        }
    SEL plane = sel_registerName("setKeyplaneName:");
    SEL hit = sel_registerName("keyHitTest:");
    Method set_plane = cls ? class_getInstanceMethod(cls, plane) : NULL;
    Method key_hit = cls ? class_getInstanceMethod(cls, hit) : NULL;
    const char *touches[2] = {"touchDown:executionContext:", "touchUp:executionContext:"};
    for (unsigned i = 0; i < 2; i++) {
        Method touch = cls ? class_getInstanceMethod(cls, sel_registerName(touches[i])) : NULL;
        if (touch && strcmp(method_getTypeEncoding(touch), "v32@0:8@16@24") == 0) {
            original_touch[i] = (void *)method_getImplementation(touch);
            class_replaceMethod(cls, sel_registerName(touches[i]), i ? (IMP)trace_touch_up : (IMP)trace_touch_down,
                method_getTypeEncoding(touch));
        }
    }
    if (set_plane && strcmp(method_getTypeEncoding(set_plane), "v24@0:8@16") == 0) {
        original_keyplane_name = (void *)method_getImplementation(set_plane);
        class_replaceMethod(cls, plane, (IMP)trace_keyplane_name, method_getTypeEncoding(set_plane));
        say("kbtrace: %s: keyplane observation installed\n", getprogname());
    }
    if (key_hit && strcmp(method_getTypeEncoding(key_hit), "@32@0:8{CGPoint=dd}16") == 0) {
        original_keyboard_hit = (void *)method_getImplementation(key_hit);
        class_replaceMethod(cls, hit, (IMP)trace_keyboard_hit, method_getTypeEncoding(key_hit));
        say("kbtrace: %s: lower-left geometry observation installed\n", getprogname());
    }
}

static IMP
keyboard_size_original(id self, SEL selector, BOOL class_method)
{
    Class current = class_method ? (Class)self : object_getClass(self);
    for (; current; current = class_getSuperclass(current))
        for (unsigned i = 0; i < keyboard_size_hook_count; ++i)
            if (keyboard_size_hooks[i].owner == current && keyboard_size_hooks[i].selector == selector &&
                keyboard_size_hooks[i].class_method == class_method)
                return keyboard_size_hooks[i].original;
    abort();
}

static CGSize
native_keyboard_size(CGSize size)
{
    if (size.width >= 320 && size.width <= 420 && size.height > 100 && size.height < 500) {
        double before = size.width;
        size.width = logical_width();
        if (__atomic_fetch_add(&keyboard_size_calls, 1, __ATOMIC_RELAXED) < 16)
            say("fb_nativeui: %s: keyboard size provider %.3fx%.3f -> %.3fx%.3f\n",
                getprogname(), before, size.height, size.width, size.height);
    }
    return size;
}

static CGSize keyboard_class_size0(id self, SEL selector)
{
    return native_keyboard_size(((CGSize (*)(id, SEL))keyboard_size_original(self, selector, YES))(self, selector));
}
static CGSize keyboard_class_size1(id self, SEL selector, long orientation)
{
    return native_keyboard_size(((CGSize (*)(id, SEL, long))keyboard_size_original(self, selector, YES))(self, selector, orientation));
}
static CGSize keyboard_class_size2(id self, SEL selector, long orientation, BOOL first)
{
    return native_keyboard_size(((CGSize (*)(id, SEL, long, BOOL))keyboard_size_original(self, selector, YES))(self, selector, orientation, first));
}
static CGSize keyboard_class_size3(id self, SEL selector, long orientation, BOOL first, BOOL second)
{
    return native_keyboard_size(((CGSize (*)(id, SEL, long, BOOL, BOOL))keyboard_size_original(self, selector, YES))(self, selector, orientation, first, second));
}
static CGSize keyboard_instance_size1(id self, SEL selector, long orientation)
{
    return native_keyboard_size(((CGSize (*)(id, SEL, long))keyboard_size_original(self, selector, NO))(self, selector, orientation));
}

static void
collect_keyboard_size(Class owner, const char *name, unsigned arity, BOOL class_method)
{
    if (!owner || keyboard_size_hook_count >= 16) return;
    SEL selector = sel_registerName(name);
    Method method = class_method ? class_getClassMethod(owner, selector) : class_getInstanceMethod(owner, selector);
    static const char *abi[] = {"{CGSize=dd}16@0:8", "{CGSize=dd}24@0:8q16",
                               "{CGSize=dd}28@0:8q16B24", "{CGSize=dd}32@0:8q16B24B28"};
    if (!method || arity > 3 || strcmp(method_getTypeEncoding(method), abi[arity]) != 0) return;
    keyboard_size_hooks[keyboard_size_hook_count++] = (struct KeyboardSizeHook){
        owner, selector, method_getImplementation(method), arity, class_method };
}

static BOOL
same_native_pixel(double first, double second)
{
    if (!isfinite(first) || !isfinite(second)) return NO;
    return fabs(first-second) < 0.0000001 || nearbyint(first*native_density()) == nearbyint(second*native_density());
}

static BOOL
native_emoji_invalidation(id self, SEL selector, id preferred, id original)
{
    BOOL result = original_emoji_invalidation(self, selector, preferred, original);
    if (!result || !preferred || !original) return result;
    CGRect first = ((CGRect (*)(id, SEL))objc_msgSend)(preferred, sel_registerName("frame"));
    CGRect second = ((CGRect (*)(id, SEL))objc_msgSend)(original, sel_registerName("frame"));
    BOOL same = same_native_pixel(first.origin.x, second.origin.x) && same_native_pixel(first.origin.y, second.origin.y) &&
                same_native_pixel(first.size.width, second.size.width) && same_native_pixel(first.size.height, second.size.height);
    if (__atomic_fetch_add(&emoji_invalidation_calls, 1, __ATOMIC_RELAXED) < 24)
        say("fb_nativeui: %s: emoji preferred %.9f,%.9f %.9fx%.9f original %.9f,%.9f %.9fx%.9f same physical pixels %d\n",
            getprogname(), first.origin.x, first.origin.y, first.size.width, first.size.height,
            second.origin.x, second.origin.y, second.size.width, second.size.height, same);
    /* Retain invalidation for every actual pixel change. Avoid re-layout only
     * when fractional point arithmetic describes the same physical rectangle. */
    return same ? NO : result;
}

static void
install_native_keyboard_sizes(void)
{
    const char *owners[] = {"UIKeyboard", "UIKeyboardImpl"};
    for (unsigned i = 0; i < 2; ++i) {
        Class owner = objc_getClass(owners[i]);
        collect_keyboard_size(owner, "defaultSize", 0, YES);
        collect_keyboard_size(owner, "defaultSizeForInterfaceOrientation:", 1, YES);
        collect_keyboard_size(owner, "keyboardSizeForInterfaceOrientation:", 1, YES);
        collect_keyboard_size(owner, "sizeForInterfaceOrientation:", 1, YES);
        collect_keyboard_size(owner, "sizeForInterfaceOrientation:ignoreInputView:", 2, YES);
        collect_keyboard_size(owner, "sizeForInterfaceOrientation:includingAssistantBar:ignoreInputView:", 3, YES);
    }
    collect_keyboard_size(objc_getClass("UIKeyboardImpl"), "defaultSizeForInterfaceOrientation:", 1, NO);
    for (unsigned i = 0; i < keyboard_size_hook_count; ++i) {
        struct KeyboardSizeHook *hook = &keyboard_size_hooks[i];
        IMP implementation = (IMP)keyboard_instance_size1;
        if (hook->class_method) {
            const IMP handlers[] = {(IMP)keyboard_class_size0, (IMP)keyboard_class_size1,
                                    (IMP)keyboard_class_size2, (IMP)keyboard_class_size3};
            implementation = handlers[hook->arity];
        }
        Class target = hook->class_method ? object_getClass(hook->owner) : hook->owner;
        const char *abi[] = {"{CGSize=dd}16@0:8", "{CGSize=dd}24@0:8q16",
                            "{CGSize=dd}28@0:8q16B24", "{CGSize=dd}32@0:8q16B24B28"};
        class_replaceMethod(target, hook->selector, implementation, abi[hook->arity]);
    }
    Class layout = objc_getClass("UIKeyboardEmojiLayout");
    SEL invalidation = sel_registerName("shouldInvalidateLayoutForPreferredLayoutAttributes:withOriginalAttributes:");
    Method method = layout ? class_getInstanceMethod(layout, invalidation) : NULL;
    if (method && strcmp(method_getTypeEncoding(method), "B32@0:8@16@24") == 0) {
        original_emoji_invalidation = (void *)method_getImplementation(method);
        class_replaceMethod(layout, invalidation, (IMP)native_emoji_invalidation, method_getTypeEncoding(method));
    }
    say("fb_nativeui: %s: %u keyboard size providers installed; UIKit geometry setters unchanged\n", getprogname(), keyboard_size_hook_count);
}

#pragma once

#include "dejavu_hook_api.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/**
 * ============================================================================
 * Dejavu Hook 宏库 - 带 dj_ 前缀，兼容 TinyCC
 * 
 * 设计原则：
 * 1. 小写函数式名称：dj_hook_log(), dj_get_arg_int() 等
 * 2. dj_ 前缀清晰表明这是宏，而非真实函数
 * 3. 依赖 TinyCC/GCC 的 GNU C 扩展（statement expression 和可变参数逗号吞并）
 * 4. 参数验证和错误处理内置于宏中
 * ============================================================================
 */

/* ========================================================================== */
/* 基础日志宏 */
/* ========================================================================== */

/**
 * dj_hook_log - 格式化日志输出
 * @fmt: printf 风格的格式字符串
 * @...: 可变参数
 */
#define dj_hook_log(fmt, ...) \
    do { \
        char dj_util_buf[256]; \
        snprintf(dj_util_buf, sizeof(dj_util_buf), fmt, ##__VA_ARGS__); \
        dejavu_hook_log(dj_util_buf); \
    } while (0)

/**
 * dj_hook_debugf - 调试日志（需要定义 HOOK_DEBUG）
 */
#ifdef HOOK_DEBUG
#define dj_hook_debugf(fmt, ...) dj_hook_log("[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define dj_hook_debugf(fmt, ...) ((void)0)
#endif

/* ========================================================================== */
/* 参数访问宏 - 获取参数 */
/* ========================================================================== */

/**
 * dj_get_arg_int - 获取整数参数
 * @ctx: context
 * @idx: 参数索引
 * @out: 输出指针
 * 
 * 使用示例：
 *   int value = 0;
 *   dj_get_arg_int(context, 0, &value);
 */
#define dj_get_arg_int(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int((ctx), (idx), (out)))

#define dj_get_arg_long(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_long((ctx), (idx), (out)))

#define dj_get_arg_bool(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_boolean((ctx), (idx), (out)))

#define dj_get_arg_byte(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_byte((ctx), (idx), (out)))

#define dj_get_arg_float(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_float((ctx), (idx), (out)))

#define dj_get_arg_double(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_double((ctx), (idx), (out)))

#define dj_get_arg_object(ctx, idx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_object((ctx), (idx), (out)))

/**
 * dj_get_arg_string - 获取字符串参数
 * @ctx: context
 * @idx: 参数索引
 * @buf: 缓冲区指针
 * @cap: 缓冲区容量
 * 
 * 返回实际字符串长度（不含 \0）
 * 
 * 使用示例：
 *   char url[256];
 *   size_t len = dj_get_arg_string(context, 0, url, sizeof(url));
 *   if (len > 0) dj_hook_log("URL: %s", url);
 */
#define dj_get_arg_string(ctx, idx, buf, cap) \
    ({ \
        size_t dj_util_size = 0; \
        int dj_util_status = dejavu_hook_get_arg_string_mutf8((ctx), (idx), (buf), (cap), &dj_util_size); \
        if (dj_util_status != DEJAVU_HOOK_OK) { \
            dj_hook_log("Failed to read string arg %u", (idx)); \
            dj_util_size = 0; \
        } \
        dj_util_size; \
    })

/* ========================================================================== */
/* 参数修改宏 - 设置参数（仅在 before_hook 中可用） */
/* ========================================================================== */

/**
 * dj_set_arg_int - 修改整数参数
 */
#define dj_set_arg_int(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_int((ctx), (idx), (val)))

#define dj_set_arg_long(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_long((ctx), (idx), (val)))

#define dj_set_arg_bool(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_boolean((ctx), (idx), (val)))

#define dj_set_arg_float(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_float((ctx), (idx), (val)))

#define dj_set_arg_double(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_double((ctx), (idx), (val)))

#define dj_set_arg_object(ctx, idx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_object((ctx), (idx), (val)))

/**
 * dj_set_arg_string - 修改字符串参数
 * @ctx: context
 * @idx: 参数索引
 * @str: 新的字符串
 * @len: 字符串长度（不含 \0）
 */
#define dj_set_arg_string(ctx, idx, str, len) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_string_mutf8((ctx), (idx), (str), (len)))

/* ========================================================================== */
/* 返回值访问宏 - 获取返回值（仅在 after_hook ��可用） */
/* ========================================================================== */

/**
 * dj_get_result_int - 获取返回值（整数）
 */
#define dj_get_result_int(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int((ctx), (out)))

#define dj_get_result_long(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_long((ctx), (out)))

#define dj_get_result_bool(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_boolean((ctx), (out)))

#define dj_get_result_float(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_float((ctx), (out)))

#define dj_get_result_double(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_double((ctx), (out)))

#define dj_get_result_object(ctx, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_object((ctx), (out)))

/**
 * dj_get_result_string - 获取返回值（字符串）
 */
#define dj_get_result_string(ctx, buf, cap) \
    ({ \
        size_t dj_util_size = 0; \
        int dj_util_status = dejavu_hook_get_result_string_mutf8((ctx), (buf), (cap), &dj_util_size); \
        if (dj_util_status != DEJAVU_HOOK_OK) { \
            dj_hook_log("Failed to read result string"); \
            dj_util_size = 0; \
        } \
        dj_util_size; \
    })

/* ========================================================================== */
/* 返回值修改宏 - 设置返回值 */
/* ========================================================================== */

/**
 * dj_set_result_int - 设置返回值（整数）
 * 在 before_hook 中使用时表示提前返回；在 after_hook 中表示修改返回值
 */
#define dj_set_result_int(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_int((ctx), (val)))

#define dj_set_result_long(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_long((ctx), (val)))

#define dj_set_result_bool(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_boolean((ctx), (val)))

#define dj_set_result_float(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_float((ctx), (val)))

#define dj_set_result_double(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_double((ctx), (val)))

#define dj_set_result_object(ctx, val) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_object((ctx), (val)))

#define dj_set_result_string(ctx, str, len) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_string_mutf8((ctx), (str), (len)))

#define dj_set_result_void(ctx) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_void((ctx)))

/**
 * dj_set_result_null - 设置返回值为 null
 */
#define dj_set_result_null(ctx) \
    DEJAVU_HOOK_TRY(dejavu_hook_set_result_object((ctx), NULL))

/* ========================================================================== */
/* 快速返回宏 - 早期返回（提前返回） */
/* ========================================================================== */

/**
 * dj_return_int - 提前返回整数
 * 使用示例：
 *   if (error) dj_return_int(context, -1);
 */
#define dj_return_int(ctx, val) \
    do { \
        int dj_util_return_value = (int)(val); \
        dj_hook_log("Early return: %d", dj_util_return_value); \
        return dejavu_hook_return_int((ctx), dj_util_return_value); \
    } while (0)

#define dj_return_long(ctx, val) \
    do { \
        long long dj_util_return_value = (long long)(val); \
        dj_hook_log("Early return: %lld", dj_util_return_value); \
        return dejavu_hook_return_long((ctx), dj_util_return_value); \
    } while (0)

#define dj_return_bool(ctx, val) \
    do { \
        int dj_util_return_value = !!(val); \
        dj_hook_log("Early return: %s", dj_util_return_value ? "true" : "false"); \
        return dejavu_hook_return_boolean((ctx), dj_util_return_value); \
    } while (0)

#define dj_return_null(ctx) \
    do { \
        dj_hook_log("Early return: null"); \
        return dejavu_hook_return_null((ctx)); \
    } while (0)

#define dj_return_void(ctx) \
    do { \
        dj_hook_log("Early return: void"); \
        return dejavu_hook_return_void((ctx)); \
    } while (0)

/* ========================================================================== */
/* 条件判断宏 - 快速条件检查 + 返回 */
/* ========================================================================== */

/**
 * dj_if_eq - 条件相等时提前返回
 * 使用示例：
 *   dj_if_eq(context, status, 0, -1);  // 如果 status == 0，返回 -1
 */
#define dj_if_eq(ctx, value, expected, return_val) \
    do { if ((value) == (expected)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_ne(ctx, value, expected, return_val) \
    do { if ((value) != (expected)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_lt(ctx, value, threshold, return_val) \
    do { if ((value) < (threshold)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_le(ctx, value, threshold, return_val) \
    do { if ((value) <= (threshold)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_gt(ctx, value, threshold, return_val) \
    do { if ((value) > (threshold)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_ge(ctx, value, threshold, return_val) \
    do { if ((value) >= (threshold)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_null(ctx, obj, return_val) \
    do { if ((obj) == NULL) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_not_null(ctx, obj, return_val) \
    do { if ((obj) != NULL) { dj_return_int((ctx), (return_val)); } } while (0)

/**
 * dj_if_in_range - 值在范围内时返回
 * 使用示例：
 *   dj_if_in_range(context, age, 0, 120, -1);
 */
#define dj_if_in_range(ctx, value, min, max, return_val) \
    do { if ((value) >= (min) && (value) <= (max)) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_out_of_range(ctx, value, min, max, return_val) \
    do { if ((value) < (min) || (value) > (max)) { dj_return_int((ctx), (return_val)); } } while (0)

/* ========================================================================== */
/* 字符串条件宏 */
/* ========================================================================== */

/**
 * dj_if_streq - 字符串相等时返回
 * 使用示例：
 *   dj_if_streq(context, str, "admin", -1);
 */
#define dj_if_streq(ctx, str, expected, return_val) \
    do { if (strcmp((str), (expected)) == 0) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_strne(ctx, str, expected, return_val) \
    do { if (strcmp((str), (expected)) != 0) { dj_return_int((ctx), (return_val)); } } while (0)

/**
 * dj_if_contains - 字符串包含子串时返回
 * 使用示例：
 *   dj_if_contains(context, url, "blocked.com", -1);
 */
#define dj_if_contains(ctx, str, substr, return_val) \
    do { if (strstr((str), (substr)) != NULL) { dj_return_int((ctx), (return_val)); } } while (0)

#define dj_if_not_contains(ctx, str, substr, return_val) \
    do { if (strstr((str), (substr)) == NULL) { dj_return_int((ctx), (return_val)); } } while (0)

/* ========================================================================== */
/* 上下文信息宏 */
/* ========================================================================== */

/**
 * dj_is_before - 检查是否在 before_hook 阶段
 */
#define dj_is_before(ctx) \
    (dejavu_hook_phase((ctx)) == DEJAVU_HOOK_PHASE_BEFORE)

/**
 * dj_is_after - 检查是否在 after_hook 阶段
 */
#define dj_is_after(ctx) \
    (dejavu_hook_phase((ctx)) == DEJAVU_HOOK_PHASE_AFTER)

/**
 * dj_get_arg_count - 获取方法参数个数
 */
#define dj_get_arg_count(ctx) \
    dejavu_hook_arg_count((ctx))

/**
 * dj_get_receiver - 获取接收者对象（this）
 * 对于静态方法返回 NULL
 */
#define dj_get_receiver(ctx) \
    dejavu_hook_this_object((ctx))

/**
 * dj_get_hook_id - 获取 hook 的唯一 ID
 */
#define dj_get_hook_id(ctx) \
    dejavu_hook_id((ctx))

/* ========================================================================== */
/* 类型检查宏 */
/* ========================================================================== */

/**
 * dj_instanceof - 检查对象是否是特定类型
 * 使用示例：
 *   int is_activity = 0;
 *   dj_instanceof(context, receiver, "android/app/Activity", &is_activity);
 *   if (is_activity) { ... }
 */
#define dj_instanceof(ctx, obj, class_name, out) \
    DEJAVU_HOOK_TRY(dejavu_hook_is_instance_of((ctx), (obj), (class_name), (out)))

/**
 * dj_check_type - 检查类型（返回布尔值）
 * 使用示例：
 *   if (dj_check_type(context, obj, "android/app/Activity")) { ... }
 */
#define dj_check_type(ctx, obj, class_name) \
    ({ \
        int dj_util_result = 0; \
        dejavu_hook_is_instance_of((ctx), (obj), (class_name), &dj_util_result); \
        dj_util_result; \
    })

/**
 * dj_if_instanceof - 是指定类型时返回
 */
#define dj_if_instanceof(ctx, obj, class_name, return_val) \
    do { if (dj_check_type((ctx), (obj), (class_name))) { dj_return_int((ctx), (return_val)); } } while (0)

/* ========================================================================== */
/* 统计和计数宏 */
/* ========================================================================== */

/**
 * dj_counter - 定义一个原子计数器（全局静态）
 * 使用示例：
 *   dj_counter(call_count);
 *   dj_inc(call_count);
 */
#define dj_counter(name) \
    static _Atomic(long long) dj_util_counter_##name = 0

#define dj_inc(name) \
    atomic_fetch_add(&dj_util_counter_##name, 1)

#define dj_add(name, delta) \
    atomic_fetch_add(&dj_util_counter_##name, (delta))

#define dj_get_count(name) \
    atomic_load(&dj_util_counter_##name)

#define dj_reset_count(name) \
    atomic_store(&dj_util_counter_##name, 0)

/**
 * dj_log_count - 记录计数值
 */
#define dj_log_count(name, fmt) \
    dj_hook_log(fmt, dj_get_count(name))

/* ========================================================================== */
/* 采样宏 */
/* ========================================================================== */

/**
 * dj_sample - 采样检查（每 N 次返回一次 true）
 * 使用示例：
 *   if (dj_sample(100)) {  // 每 100 次采样一次
 *       dj_hook_log("Sampled call");
 *   }
 */
#define dj_sample(n) \
    ({ \
        static _Atomic(int) dj_util_sample_counter = 0; \
        int dj_util_period = (n); \
        int dj_util_sample = 0; \
        if (dj_util_period > 0) { \
            dj_util_sample = (atomic_fetch_add(&dj_util_sample_counter, 1) % dj_util_period) == 0; \
        } \
        dj_util_sample; \
    })

/**
 * dj_if_sampled - 采样后执行代码
 */
#define dj_if_sampled(n) \
    if (dj_sample(n))

/* ========================================================================== */
/* 错误处理宏 */
/* ========================================================================== */

/**
 * dj_try - 执行表达式，失败时记录日志并返回错误状态
 * 使用示例：
 *   dj_try(dejavu_hook_get_arg_int(context, 0, &value), "Failed to get arg");
 */
#define dj_try(expression, msg) \
    do { \
        int dj_util_status = (expression); \
        if (dj_util_status != DEJAVU_HOOK_OK) { \
            dj_hook_log("[ERROR] %s (status=%d)", (msg), dj_util_status); \
            return dj_util_status; \
        } \
    } while (0)

/**
 * dj_try_safe - 在 after_hook 中安全地处理错误（失败时不中断）
 */
#define dj_try_safe(expression, msg) \
    do { \
        int dj_util_status = (expression); \
        if (dj_util_status != DEJAVU_HOOK_OK) { \
            dj_hook_log("[WARNING] %s (status=%d)", (msg), dj_util_status); \
        } \
    } while (0)

/* ========================================================================== */
/* 组合模式宏 - 常见业务逻辑 */
/* ========================================================================== */

/**
 * dj_validate_range - 参数范围验证
 * 使用示例：
 *   dj_validate_range(context, 0, 0, 100, -1);  // 参数 0 应在 0-100 范围内
 */
#define dj_validate_range(ctx, arg_idx, min, max, return_val) \
    do { \
        int dj_util_arg = 0; \
        dj_get_arg_int((ctx), (arg_idx), &dj_util_arg); \
        if (dj_util_arg < (min) || dj_util_arg > (max)) { \
            dj_hook_log("Arg %u out of range: %d [%d, %d]", \
                       (arg_idx), dj_util_arg, (min), (max)); \
            dj_return_int((ctx), (return_val)); \
        } \
    } while (0)

/**
 * dj_validate_string - 字符串参数验证
 */
#define dj_validate_string(ctx, arg_idx, return_val) \
    do { \
        char dj_util_str[256] = {0}; \
        size_t dj_util_len = dj_get_arg_string((ctx), (arg_idx), dj_util_str, sizeof(dj_util_str)); \
        if (dj_util_len == 0) { \
            dj_hook_log("Arg %u is empty string", (arg_idx)); \
            dj_return_int((ctx), (return_val)); \
        } \
    } while (0)

/**
 * dj_log_all_args - 记录所有参数（调试用）
 * 仅支持整数参数
 */
#define dj_log_all_args(ctx) \
    do { \
        unsigned int dj_util_count = dj_get_arg_count((ctx)); \
        dj_hook_log("Total args: %u", dj_util_count); \
        unsigned int dj_util_i; \
        int dj_util_arg; \
        for (dj_util_i = 0; dj_util_i < dj_util_count && dj_util_i < 10; ++dj_util_i) { \
            if (dejavu_hook_get_arg_int((ctx), dj_util_i, &dj_util_arg) == DEJAVU_HOOK_OK) { \
                dj_hook_log("  arg[%u] = %d", dj_util_i, dj_util_arg); \
            } \
        } \
    } while (0)

# cmem 内存安全配置管理

## 问题分析

当前cmem构建系统存在以下内存安全配置问题：

1. **Release构建不明确禁用安全标志**：CMake Release构建没有显式配置MP_FLAG_DEBUG_CANARY和MP_FLAG_POISON_ON_FREE
2. **跨系统配置不一致**：Makefile和CMake构建系统对安全标志的处理方式不同
3. **安全标志隐式启用**：安全标志只被明确设置在测试代码中，Release构建没有明确禁用
4. **构建类型混淆**：存在CMAKE_BUILD_TYPE（Release/Debug/ASan等）和CONFIG（make/debug/asan等）两种配置体系

## 解决方案

### 1. 引入内存安全配置选项

#### cmem.h中添加安全标志支持

```c
/**
 * @brief 内存安全配置选项
 *
 * 控制内存池的安全检查行为。这些选项影响调试信息收集、
 * 内存保护和性能的平衡。
 */
typedef enum {
    MP_SAFETY_DEFAULT = 0,           /**< 默认安全配置，根据构建类型自动调整 */
    MP_SAFETY_MINIMAL = 1,           /**< 最小安全配置，优先考虑性能 */
    MP_SAFETY_BALANCED = 2,          /**< 平衡安全与性能的配置 */
    MP_SAFETY_MAXIMAL = 3,           /**< 最大安全配置，优先考虑安全 */
} mp_safety_level_t;

/**
 * @brief 安全标志集合
 *
 * 表示当前启用的安全检查类型。这是一个位字段集合，
 * 可以包含多个标志，表示同时启用的安全检查。
 */
typedef enum {
    MP_SAFETY_NONE = 0,              /**< 不启用任何安全检查 */
    MP_SAFETY_CANARY = (1 << 0),     /**< 启用Canary检查，检测缓冲区溢出 */
    MP_SAFETY_POISON = (1 << 1),     /**< 启用Poison检查，检测Use-After-Free */
    MP_SAFETY_CANARY_POISON = (MP_SAFETY_CANARY | MP_SAFETY_POISON), /**< 同时启用Canary和Poison */
    MP_SAFETY_ALL = (MP_SAFETY_CANARY | MP_SAFETY_POISON), /**< 所有安全检查 */
} mp_safety_flags_t;
```

#### 根据构建类型自动选择安全标志

```c
/**
 * @brief 根据构建类型自动选择安全标志
 *
 * @param build_type 构建类型字符串（如"Debug"、"Release"等）
 * @return 安全标志
 */
static inline mp_safety_flags_t mp_get_safety_flags_for_build_type(const char* build_type)
{
    if (build_type == NULL) return MP_SAFETY_BALANCED;
    
    // 统一处理构建类型名称
    const char* normalized = build_type;
    
    if (strcmp(normalized, "Debug") == 0 ||
        strcmp(normalized, "debug") == 0 ||
        strcmp(normalized, "DBG") == 0) {
        return MP_SAFETY_CANARY_POISON;
    }
    
    if (strcmp(normalized, "ASan") == 0 ||
        strcmp(normalized, "asan") == 0 ||
        strcmp(normalized, "AddressSanitizer") == 0) {
        return MP_SAFETY_CANARY_POISON;
    }
    
    if (strcmp(normalized, "TSan") == 0 ||
        strcmp(normalized, "tsan") == 0 ||
        strcmp(normalized, "ThreadSanitizer") == 0) {
        return MP_SAFETY_CANARY_POISON;
    }
    
    if (strcmp(normalized, "UBSan") == 0 ||
        strcmp(normalized, "ubsan") == 0 ||
        strcmp(normalized, "UndefinedSanitizer") == 0) {
        return MP_SAFETY_CANARY_POISON;
    }
    
    // 对于Release、Coverage和未知类型，启用基本的Canary检查
    return MP_SAFETY_CANARY;
}

/**
 * @brief 根据安全级别自动选择安全标志
 *
 * @param level 安全级别
 * @param build_type 构建类型（用于调整）
 * @return 安全标志
 */
static inline mp_safety_flags_t mp_get_safety_flags_for_level(mp_safety_level_t level, const char* build_type)
{
    mp_safety_flags_t flags = MP_SAFETY_DEFAULT;
    
    switch (level) {
        case MP_SAFETY_MINIMAL:
            // 最小安全配置，只启用基本的Canary检查
            flags = MP_SAFETY_CANARY;
            break;
            
        case MP_SAFETY_BALANCED:
            // 平衡配置，根据构建类型调整
            flags = mp_get_safety_flags_for_build_type(build_type);
            break;
            
        case MP_SAFETY_MAXIMAL:
            // 最大安全配置，启用所有安全检查
            flags = MP_SAFETY_CANARY_POISON;
            break;
            
        case MP_SAFETY_DEFAULT:
            // 默认配置，根据构建类型调整
            flags = mp_get_safety_flags_for_build_type(build_type);
            break;
            
        default:
            flags = MP_SAFETY_BALANCED;
            break;
    }
    
    return flags;
}
```

#### 安全标志到内存池标志的映射

```c
/**
 * @brief 安全标志到内存池标志的映射
 *
 * @param safety_flags 安全标志
 * @return 内存池标志
 */
static inline mp_flags_t mp_safety_flags_to_pool_flags(mp_safety_flags_t safety_flags)
{
    mp_flags_t pool_flags = MP_FLAG_DEFAULT;
    
    switch (safety_flags) {
        case MP_SAFETY_NONE:
            // 不启用任何安全检查
            break;
            
        case MP_SAFETY_CANARY:
            // 只启用Canary检查
            pool_flags |= MP_FLAG_DEBUG_CANARY;
            break;
            
        case MP_SAFETY_CANARY_POISON:
            // 同时启用Canary和Poison检查
            pool_flags |= MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE;
            break;
            
        case MP_SAFETY_ALL:
            // 所有安全检查
            pool_flags |= MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE;
            break;
            
        default:
            // 默认行为：Release启用基本的Canary检查，Debug启用完整的Canary+Poison
            pool_flags |= MP_FLAG_DEBUG_CANARY;
            break;
    }
    
    return pool_flags;
}
```

### 2. 统一构建系统配置

#### CMakeLists.txt增强版

```cmake
# 安全级别选项
# 可选值：MINIMAL, BALANCED, MAXIMAL, DEFAULT
# 默认值：BALANCED
# 这将影响CMake构建类型（如Release、Debug等）的安全标志选择
set(CMEM_SAFETY_LEVEL "BALANCED" CACHE STRING "内存安全级别")

# 强制覆盖安全级别的安全标志（用于特殊情况）
# 例如：set(CMEM_FORCE_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE")

# 根据安全级别自动配置安全标志
if(CMEM_SAFETY_LEVEL STREQUAL "MINIMAL")
    set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY")
elseif(CMEM_SAFETY_LEVEL STREQUAL "MAXIMAL")
    set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE")
elseif(CMEM_SAFETY_LEVEL STREQUAL "BALANCED")
    # 根据构建类型平衡安全标志
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "ASan" OR
       CMAKE_BUILD_TYPE STREQUAL "TSan" OR CMAKE_BUILD_TYPE STREQUAL "UBSan")
        set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE")
    else()
        # Release和未知类型，启用基本的Canary检查
        set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY")
    endif()
else() # DEFAULT
    # 默认行为，根据构建类型调整
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "ASan" OR
       CMAKE_BUILD_TYPE STREQUAL "TSan" OR CMAKE_BUILD_TYPE STREQUAL "UBSan")
        set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE")
    else()
        # Release和未知类型
        set(CMEM_SAFETY_FLAGS "-DMP_FLAG_DEBUG_CANARY")
    endif()
endif()

# 如果用户强制设置安全标志，则使用强制设置
if(DEFINED CMEM_FORCE_SAFETY_FLAGS)
    set(CMEM_SAFETY_FLAGS "${CMEM_FORCE_SAFETY_FLAGS}")
endif()

# 添加安全标志到编译中
add_compile_options(${CMEM_SAFETY_FLAGS})

# 为所有目标添加安全标志
function(apply_memory_safety_to_target target_name)
    target_compile_options(${target_name} PRIVATE ${CMEM_SAFETY_FLAGS})
endfunction()

# 应用到所有目标
apply_memory_safety_to_target(mp)
apply_memory_safety_to_target(mp_shared)
apply_memory_safety_to_target(unit_tests)
apply_memory_safety_to_target(advanced_tests)
apply_memory_safety_to_target(cpp_tests)
apply_memory_safety_to_target(benchmark)
apply_memory_safety_to_target(example_basic)
apply_memory_safety_to_target(example_embedded)
apply_memory_safety_to_target(stress_test)
```

#### 统一Makefile配置

```makefile
# 安全级别配置
ifdef CMEM_SAFETY_LEVEL
    CMEM_CURRENT_SAFETY_LEVEL = $(CMEM_SAFETY_LEVEL)
else
    CMEM_CURRENT_SAFETY_LEVEL = BALANCED
endif

# 根据安全级别计算安全标志
case $(CMEM_CURRENT_SAFETY_LEVEL) in \
    MINIMAL)
        CMEM_SAFETY_FLAGS = -DMP_FLAG_DEBUG_CANARY
        ;;
    MAXIMAL)
        CMEM_SAFETY_FLAGS = -DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE
        ;;
    BALANCED|*) # 平衡
        case $(CONFIG) in \
            debug|asan|tsan|ubsan)
                CMEM_SAFETY_FLAGS = -DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE
                ;;
            *)
                # Release和未知类型，启用基本的Canary检查
                CMEM_SAFETY_FLAGS = -DMP_FLAG_DEBUG_CANARY
                ;;
        esac
        ;;
esac

# 如果用户强制设置安全标志，则使用强制设置
ifdef CMEM_FORCE_SAFETY_FLAGS
    CMEM_SAFETY_FLAGS = $(CMEM_FORCE_SAFETY_FLAGS)
endif

# 添加到编译标志
CFLAGS += $(CMEM_SAFETY_FLAGS)
CXXFLAGS += $(CMEM_SAFETY_FLAGS)

# 显示安全标志配置
echo "=== cmem 安全标志配置 ==="
echo "安全级别: $(CMEM_CURRENT_SAFETY_LEVEL)"
echo "安全标志: $(filter -D%, $(CFLAGS))"
echo "=========================="
```

### 3. 更新安全标志宏定义

#### cmem_internal.h中添加安全标志支持

```c
/**
 * @brief 获取当前构建类型
 *
 * @return 构建类型字符串（如"Debug"、"Release"等）
 */
static inline const char* mp_get_current_build_type(void)
{
#ifdef CMAKE_BUILD_TYPE
    return CMAKE_BUILD_TYPE;
#elif defined(CONFIG)
    if (strcmp(CONFIG, "debug") == 0) return "Debug";
    if (strcmp(CONFIG, "asan") == 0) return "ASan";
    if (strcmp(CONFIG, "tsan") == 0) return "TSan";
    if (strcmp(CONFIG, "ubsan") == 0) return "UBSan";
    if (strcmp(CONFIG, "release") == 0) return "Release";
    if (strcmp(CONFIG, "make") == 0) return "Release";
    return "Unknown";
#else
    return "Unknown";
#endif
}

/**
 * @brief 获取安全的内存池标志
 *
 * @param requested_flags 用户请求的安全标志
 * @return 安全的内存池标志
 */
static inline mp_flags_t mp_get_safe_pool_flags(mp_flags_t requested_flags)
{
    mp_flags_t final_flags = requested_flags;
    
    // 根据构建类型调整安全标志
    const char* build_type = mp_get_current_build_type();
    
    // 获取根据构建类型推荐的安全标志
    mp_safety_flags_t recommended_safety = mp_get_safety_flags_for_build_type(build_type);
    
    // 根据推荐的安全标志调整最终标志
    if (recommended_safety == MP_SAFETY_NONE) {
        // 不启用任何安全检查
        final_flags &= ~(MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE);
    } else if (recommended_safety == MP_SAFETY_CANARY) {
        // 只启用Canary检查
        final_flags |= MP_FLAG_DEBUG_CANARY;
        final_flags &= ~MP_FLAG_POISON_ON_FREE;
    } else if (recommended_safety == MP_SAFETY_CANARY_POISON) {
        // 同时启用Canary和Poison检查
        final_flags |= MP_FLAG_DEBUG_CANARY | MP_FLAG_POISON_ON_FREE;
    }
    
    return final_flags;
}

/**
 * @brief 检查安全标志是否已设置
 *
 * @param pool 内存池
 * @param flag 需要检查的标志
 * @return true if the flag is set
 */
static inline bool mp_has_safety_flag(memory_pool_t *pool, mp_flags_t flag)
{
    if (!pool) return false;
    
    uint32_t flags = *(const uint32_t *)pool;
    return (flags & flag) != 0;
}

/**
 * @brief 安全创建内存池
 *
 * @param initial_capacity 初始容量
 * @param requested_flags 用户请求的安全标志
 * @return 新创建的内存池
 */
static memory_pool_t *mp_create_safe(size_t initial_capacity, mp_flags_t requested_flags)
{
    mp_flags_t safe_flags = mp_get_safe_pool_flags(requested_flags);
    return mp_create_custom(initial_capacity, safe_flags, NULL);
}
```

#### cmem.h中更新安全标志宏

```c
/**
 * @brief 根据安全级别获取标志
 *
 * @param level 安全级别
 * @param flags 安全标志
 * @return 合并的标志
 */
#define MP_GET_SAFE_FLAGS(level, flags) \
    _MP_GET_SAFE_FLAGS_impl(level, flags, \
                           CMEM_SAFETY_LEVEL, CMEM_FORCE_SAFETY_FLAGS)

/**
 * @brief 检查是否为Release构建类型
 *
 * @return true if Release构建
 */
#define MP_IS_RELEASE_BUILD() \
    (defined(CMAKE_BUILD_TYPE) ? (strcmp(CMAKE_BUILD_TYPE, "Release") == 0) : \
     (defined(CONFIG) ? (strcmp(CONFIG, "release") == 0 || \
                         strcmp(CONFIG, "make") == 0) : false))

/**
 * @brief 检查是否为Debug构建类型
 *
 * @return true if Debug构建
 */
#define MP_IS_DEBUG_BUILD() \
    (defined(CMAKE_BUILD_TYPE) ? (strcmp(CMAKE_BUILD_TYPE, "Debug") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "ASan") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "TSan") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "UBSan") == 0) : \
     (defined(CONFIG) ? (strcmp(CONFIG, "debug") == 0 || \
                         strcmp(CONFIG, "asan") == 0 || \
                         strcmp(CONFIG, "tsan") == 0 || \
                         strcmp(CONFIG, "ubsan") == 0) : false))

/**
 * @brief 获取安全的内存池标志
 *
 * @param requested_flags 用户请求的安全标志
 * @return 安全的内存池标志
 */
#define MP_GET_SAFE_POOL_FLAGS(requested_flags) mp_get_safe_pool_flags(requested_flags)

/**
 * @brief 检查安全标志是否已设置
 *
 * @param pool 内存池
 * @param flag 需要检查的标志
 * @return true if the flag is set
 */
#define MP_HAS_SAFETY_FLAG(pool, flag) mp_has_safety_flag(pool, flag)
```

### 4. 更新安全标志创建函数

#### mp_create_safe的实现

```c
/**
 * @brief 安全创建内存池
 *
 * @param initial_capacity 初始容量
 * @param flags 安全标志
 * @return 新创建的内存池
 */
memory_pool_t *mp_create_safe(size_t initial_capacity, mp_flags_t flags)
{
    return mp_create_safe(initial_capacity, flags);
}
```

### 5. 更新安全标志宏定义

#### cmem.h中更新安全标志宏

```c
/**
 * @brief 根据安全级别获取标志
 *
 * @param level 安全级别
 * @param flags 安全标志
 * @return 合并的标志
 */
#define MP_GET_SAFE_FLAGS(level, flags) \
    _MP_GET_SAFE_FLAGS_impl(level, flags, \
                           CMEM_SAFETY_LEVEL, CMEM_FORCE_SAFETY_FLAGS)

/**
 * @brief 检查是否为Release构建类型
 *
 * @return true if Release构建
 */
#define MP_IS_RELEASE_BUILD() \
    (defined(CMAKE_BUILD_TYPE) ? (strcmp(CMAKE_BUILD_TYPE, "Release") == 0) : \
     (defined(CONFIG) ? (strcmp(CONFIG, "release") == 0 || \
                         strcmp(CONFIG, "make") == 0) : false))

/**
 * @brief 检查是否为Debug构建类型
 *
 * @return true if Debug构建
 */
#define MP_IS_DEBUG_BUILD() \
    (defined(CMAKE_BUILD_TYPE) ? (strcmp(CMAKE_BUILD_TYPE, "Debug") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "ASan") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "TSan") == 0 || \
                                 strcmp(CMAKE_BUILD_TYPE, "UBSan") == 0) : \
     (defined(CONFIG) ? (strcmp(CONFIG, "debug") == 0 || \
                         strcmp(CONFIG, "asan") == 0 || \
                         strcmp(CONFIG, "tsan") == 0 || \
                         strcmp(CONFIG, "ubsan") == 0) : false))

/**
 * @brief 获取安全的内存池标志
 *
 * @param requested_flags 用户请求的安全标志
 * @return 安全的内存池标志
 */
#define MP_GET_SAFE_POOL_FLAGS(requested_flags) mp_get_safe_pool_flags(requested_flags)

/**
 * @brief 检查安全标志是否已设置
 *
 * @param pool 内存池
 * @param flag 需要检查的标志
 * @return true if the flag is set
 */
#define MP_HAS_SAFETY_FLAG(pool, flag) mp_has_safety_flag(pool, flag)

/**
 * @brief 根据安全级别自动选择安全标志
 *
 * @param level 安全级别
 * @return 安全标志
 */
#define MP_AUTO_SAFETY_FLAGS(level) \
    _MP_AUTO_SAFETY_FLAGS_impl(level, \
                              defined(CMAKE_BUILD_TYPE) ? CMAKE_BUILD_TYPE : \
                              (defined(CONFIG) ? CONFIG : ""))
```

### 6. 更新文档和示例

#### 更新README.md

```markdown
# cmem - 通用高性能分层内存管理器

## 🚀 快速开始 - 安全配置

### 安全级别

cmem现在支持多种内存安全级别，通过`CMEM_SAFETY_LEVEL`环境变量或编译时选项配置：

| 安全级别 | 标志 | 描述 | 性能影响 | 适用场景 |
|----------|------|------|------------|------------|
| `MINIMAL` | 仅Canary | 启用基本的Canary检查 | 轻微 | 生产环境，高性能 |
| `BALANCED` | Canary + Poison | 根据构建类型自动调整安全标志 | 中等 | 典型开发，平衡安全与性能 |
| `MAXIMAL` | Canary + Poison + 所有检查 | 启用所有安全检查 | 大 | 调试、单元测试、安全关键 |
| `DEFAULT` | 根据构建类型自动选择 | 默认行为 | 智能 | 一般使用 |

### 构建配置示例

```bash
# 最小安全级别（仅Canary）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_SAFETY_LEVEL=MINIMAL

# 平衡安全级别（根据构建类型自动选择）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_SAFETY_LEVEL=BALANCED

# 最大安全级别（所有安全检查）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMEM_SAFETY_LEVEL=MAXIMAL

# 使用Makefile构建时的安全配置
make clean && make test CMEM_SAFETY_LEVEL=BALANCED
```

### 使用指南

#### 1. 确定您的安全需求

- **生产环境**：使用 `MINIMAL` 或 `BALANCED` 安全级别，启用基本的Canary检查
- **调试/开发**：使用 `MAXIMAL` 安全级别，启用完整的Canary + Poison检查
- **安全关键**：使用 `MAXIMAL` 安全级别，所有安全检查都已启用
- **高性能**：使用 `MINIMAL` 安全级别，禁用Poison检查

#### 2. 选择合适的构建类型

```bash
# 生产构建（最小安全级别）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_SAFETY_LEVEL=MINIMAL

# 调试构建（最大安全级别）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMEM_SAFETY_LEVEL=MAXIMAL

# ASan构建（最大安全级别 + ASan集成）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=ASan \
    -DCMEM_SAFETY_LEVEL=MAXIMAL
```

#### 3. 使用安全标志

```c
#include "cmem.h"
#include <stdio.h>

int main() {
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    
    // 检查安全标志
    if (mp_has_safety_flag(pool, MP_FLAG_DEBUG_CANARY)) {
        printf("已启用: Canary检查 (缓冲区溢出检测)\n");
    } else {
        printf("已禁用: Canary检查\n");
    }
    
    if (mp_has_safety_flag(pool, MP_FLAG_POISON_ON_FREE)) {
        printf("已启用: Poison检查 (Use-After-Free检测)\n");
    } else {
        printf("已禁用: Poison检查\n");
    }
    
    mp_destroy(pool);
    return 0;
}
```

#### 4. 创建安全内存池

```c
#include "cmem.h"

int main() {
    // 创建安全内存池
    memory_pool_t* pool = mp_create_safe(1024 * 1024, MP_FLAG_DEFAULT);
    
    // 使用安全池进行分配和释放
    void* ptr = mp_alloc(pool, 128);
    if (ptr) {
        // ... 使用分配的内存 ...
        mp_free(pool, ptr);
    }
    
    mp_destroy(pool);
    return 0;
}
```

## 📋 安全级别概述

| 安全级别 | 标志 | 安全检查 | 性能影响 | 适用场景 |
|----------|------|------------|------------|------------|
| `MINIMAL` | Canary | 缓冲区溢出检测 | 轻微 | 生产环境，高性能 |
| `BALANCED` | Canary + Poison | 缓冲区溢出检测 + Use-After-Free检测 | 中等 | 典型开发 |
| `MAXIMAL` | Canary + Poison + 所有检查 | 所有安全检查 | 大 | 调试、单元测试、安全关键 |

## ⚠️ 注意事项

1. **安全标志根据构建类型自动调整**，Release构建启用基本的Canary检查，Debug构建启用完整的Canary + Poison检查
2. **安全标志只能在构建时设置**，不能在运行时动态更改
3. **安全标志影响Canary和Poison检查**，不影响其他功能如Guard Pages、ASan集成等
4. **在禁用安全标志时，强烈建议使用其他安全手段**（如静态分析、单元测试等）
5. **不要混用相同类型的安全标志**

## 🎯 总结

### 核心原则

1. **安全标志根据构建类型自动调整**：Release启用基本的Canary检查，Debug启用完整的Canary + Poison检查
2. **用户可以根据实际需求调整安全级别**：MINIMAL、BALANCED或MAXIMAL
3. **安全标志只影响Canary和Poison检查**，不影响其他功能
4. **在禁用安全标志时，强烈建议使用其他安全手段**

### 构建类型与安全标志的映射关系

| 构建类型 | 默认安全标志 | 原因 |
|----------|---------------|------|
| `Debug` | Canary + Poison | 需要所有安全检查，便于调试 |
| `Release` | Canary | 平衡安全与性能，基本的Canary检查足以应对常见错误 |
| `ASan` | Canary + Poison | 需要安全检查 + ASan集成 |
| `Coverage` | Canary | 平衡安全与覆盖率 |

### 安全标志配置优先级

1. **命令行参数**（最高优先级）
2. **CMake选项**
3. **Makefile变量**
4. **环境变量**
5. **编译时定义**

### 性能影响

| 安全标志 | 性能影响 | 建议使用场景 |
|----------|----------|----------------|
| `Canary` | 轻微 | 生产环境，高性能 |
| `Canary + Poison` | 中等 | 典型开发，安全关键 |

### 安全考虑

1. **禁用安全标志时增加Use-After-Free风险**
2. **在生产环境中仍然推荐最基本的Canary检查**
3. **使用Guard Pages、ASan等其他安全手段**
4. **进行充分的单元测试和集成测试**

这种设计提供了灵活的内存安全配置选项，同时保持了简单性和一致性。用户可以根据实际需求选择合适的构建类型和安全级别，既能获得足够的安全保护，又不会过度影响性能。 🎯
```

## 📦 打包与安装

### 打包构建结果

```bash
# 打包构建产物
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_SAFETY_LEVEL=MINIMAL
make -C build package

# 打包时强制安全标志
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_FORCE_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE"
make -C build package
```

### 安装到系统

```bash
# 安装到/usr/local
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMEM_SAFETY_LEVEL=MINIMAL
make -C build install PREFIX=/usr/local

# 安装时强制安全标志
make -C build install PREFIX=/usr/local \
    CMEM_FORCE_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE"
```

## 🔧 高级用法

### 1. 跨平台安全配置

```bash
#!/bin/bash
# 跨平台安全配置脚本

SAFETY_LEVEL="${1:-BALANCED}"
BUILD_TYPE="${2:-Release}"

# 根据安全级别和构建类型配置
case "$SAFETY_LEVEL" in \
    MINIMAL)
        CMEM_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY"
        ;;
    MAXIMAL)
        CMEM_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE"
        ;;
    BALANCED|*) # 平衡
        case "$BUILD_TYPE" in \
            Debug|ASan|TSan|UBSan)
                CMEM_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY -DMP_FLAG_POISON_ON_FREE"
                ;;
            *)
                CMEM_SAFETY_FLAGS="-DMP_FLAG_DEBUG_CANARY"
                ;;
        esac
        ;;
esac

echo "使用安全级别: $SAFETY_LEVEL, 构建类型: $BUILD_TYPE"

echo "设置安全标志: $CMEM_SAFETY_FLAGS"

# 应用到构建
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMEM_SAFETY_LEVEL="$SAFETY_LEVEL"

make -C build
make -C build test
```

### 2. CI/CD集成

```yaml
# .github/workflows/ci.yml
name: cmem CI

on:
  push:
    branches: [ main ]
  pull_request:
    branches: [ main ]

jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        build_type: [Debug, Release, ASan]
        safety_level: [MINIMAL, BALANCED, MAXIMAL]
        exclude:
          # Release构建时不推荐MAXIMAL安全级别（性能影响太大）
          - build_type: Release
            safety_level: MAXIMAL
    
    steps:
    - uses: actions/checkout@v3
    
    - name: 配置CMake构建
      run: |
        cmake -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} \
            -DCMEM_SAFETY_LEVEL=${{ matrix.safety_level }}
    
    - name: 构建
      run: make -C build
    
    - name: 运行测试
      run: make -C build test
      if: matrix.build_type != 'ASan'
    
    - name: 运行ASan测试
      run: ctest --test-dir build --output-on-failure
      if: matrix.build_type == 'ASan'
```

### 3. 安全标志监控

```c
#include "cmem.h"
#include <stdio.h>

void monitor_memory_safety(memory_pool_t* pool) {
    const char* build_type = mp_get_current_build_type();
    mp_safety_level_t safety_level = MP_IS_DEBUG_BUILD() ? MP_SAFETY_MAXIMAL : MP_IS_RELEASE_BUILD() ? MP_SAFETY_MINIMAL : MP_SAFETY_BALANCED;
    
    printf("=== 内存安全状态监控 ===\n");
    printf("构建类型: %s\n", build_type);
    printf("安全级别: %d\n", safety_level);
    
    // 检查Canary标志
    if (mp_has_safety_flag(pool, MP_FLAG_DEBUG_CANARY)) {
        printf("✓ 已启用 Canary检查 (缓冲区溢出检测)\n");
    } else {
        printf("✗ 已禁用 Canary检查\n");
    }
    
    // 检查Poison标志
    if (mp_has_safety_flag(pool, MP_FLAG_POISON_ON_FREE)) {
        printf("✓ 已启用 Poison检查 (Use-After-Free检测)\n");
    } else {
        printf("✗ 已禁用 Poison检查\n");
    }
    
    // 获取统计信息
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    
    printf("\n=== 分配统计 ===\n");
    printf("总分配次数: %zu\n", stats.total_alloc_ops);
    printf("总释放次数: %zu\n", stats.total_free_ops);
    printf("活动分配数: %zu\n", stats.active_allocations);
    printf("峰值内存使用: %zu 字节\n", stats.peak_bytes);
    printf("===================\n");
}
```

## 📚 相关文档

1. **CMEM_H安全标志定义**: `include/cmem.h`中的MP_FLAG_DEBUG_CANARY和MP_FLAG_POISON_ON_FREE
2. **构建系统文档**: `AGENTS.md`中的构建系统章节
3. **安全检查文档**: `docs/en/security.md` 或 `docs/zh/security.md`
4. **性能基准**: `benchmarks/bench_main.c`，展示不同安全标志下的性能差异

## 🎯 总结

### 核心原则

1. **安全标志根据构建类型自动调整**，Release构建启用基本的Canary检查，Debug构建启用完整的Canary + Poison检查
2. **用户可以根据实际需求调整安全级别**：MINIMAL、BALANCED或MAXIMAL
3. **安全标志只影响Canary和Poison检查**，不影响其他功能
4. **在禁用安全标志时，强烈建议使用其他安全手段**

### 构建类型与安全标志的映射关系

| 构建类型 | 默认安全标志 | 原因 |
|----------|---------------|------|
| `Debug` | Canary + Poison | 需要所有安全检查，便于调试 |
| `Release` | Canary | 平衡安全与性能，基本的Canary检查足以应对常见错误 |
| `ASan` | Canary + Poison | 需要安全检查 + ASan集成 |
| `Coverage` | Canary | 平衡安全与覆盖率 |

### 安全标志配置优先级

1. **命令行参数**（最高优先级）
2. **CMake选项**
3. **Makefile变量**
4. **环境变量**
5. **编译时定义**

### 性能影响

| 安全标志 | 性能影响 | 建议使用场景 |
|----------|----------|----------------|
| `Canary` | 轻微 | 生产环境，高性能 |
| `Canary + Poison` | 中等 | 典型开发，安全关键 |

### 安全考虑

1. **禁用安全标志时增加Use-After-Free风险**
2. **在生产环境中仍然推荐最基本的Canary检查**
3. **使用Guard Pages、ASan等其他安全手段**
4. **进行充分的单元测试和集成测试**

这种设计提供了灵活的内存安全配置选项，同时保持了简单性和一致性。用户可以根据实际需求选择合适的构建类型和安全级别，既能获得足够的安全保护，又不会过度影响性能。 🎯
```

## 🚀 快速开始

### 1. 克隆项目

```bash
git clone https://github.com/quintin-lee/cmem.git
cd cmem
```

### 2. 选择安全级别

```bash
# 默认（平衡安全级别）
make clean && make test

# 最小安全级别（仅Canary）
make clean && make test CMEM_SAFETY_LEVEL=MINIMAL

# 平衡安全级别（默认）
make clean && make test CMEM_SAFETY_LEVEL=BALANCED

# 最大安全级别（所有安全检查）
make clean && make test CMEM_SAFETY_LEVEL=MAXIMAL
```

### 3. 使用CMake构建

```bash
# 最小安全级别
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMEM_SAFETY_LEVEL=MINIMAL
make -C build
make -C build test

# 平衡安全级别
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMEM_SAFETY_LEVEL=BALANCED
make -C build
make -C build test

# 最大安全级别
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMEM_SAFETY_LEVEL=MAXIMAL
make -C build
make -C build test
```

### 4. 验证构建

```bash
# 构建所有目标
make clean && make test_all

# 构建并运行基准测试
make clean && make bench

# 构建诊断工具
make clean && make tools

# 构建所有示例
make clean && make examples
```

## 📋 构建类型概述

| 构建类型 | 安全标志 | 描述 | 适用场景 |
|----------|----------|------|------------|
| `debug` | Canary + Poison | 调试版本，启用所有安全检查 | 调试、单元测试 |
| `asan` | Canary + Poison | ASan版本，安全检查 + AddressSanitizer | 内存安全检测 |
| `tsan` | Canary + Poison | TSan版本，安全检查 + ThreadSanitizer | 数据竞争检测 |
| `ubsan` | Canary + Poison | UBSan版本，安全检查 + UndefinedBehaviorSanitizer | 未定义行为检测 |
| `release` | Canary | 发布版本，基本安全检查 | 生产环境 |

## ⚠️ 注意事项

1. **安全标志根据构建类型自动调整**，Release构建启用基本的Canary检查，Debug构建启用完整的Canary + Poison检查
2. **安全标志只能在构建时设置**，不能在运行时动态更改
3. **安全标志影响Canary和Poison检查**，不影响其他功能如Guard Pages、ASan集成等
4. **在禁用安全标志时，强烈建议使用其他安全手段**（如静态分析、单元测试等）
5. **不要混用相同类型的安全标志**

## 🎯 总结

cmem现在提供了灵活的内存安全配置选项，用户可以根据实际需求选择合适的构建类型和安全级别。

- **生产环境**：使用`release`构建类型和`MINIMAL`安全级别
- **调试/测试**：使用`debug`构建类型和`MAXIMAL`安全级别
- **安全关键**：使用`san`构建类型和`MAXIMAL`安全级别

这种设计既能获得足够的安全保护，又不会过度影响性能，是cmem构建系统的重大改进。

> **提示**：如果不确定选择哪种安全级别，建议从`BALANCED`开始，根据实际需求调整。

---
*文档更新于: 2026年10月20日*
*作者: Kilo - cmem团队*

add_rules("mode.debug", "mode.release")
set_languages("cxx20")

-- 添加依赖
add_requires("gtest", "plf_nanotimer")

-- 定义 header-only 库 "ur"
target("ur")
    set_kind("headeronly")
    add_includedirs("include",{
      public = true
    })

-- 定义测试程序
target("tests")
    set_kind("binary")
    add_files("tests/signature_test.cpp", "tests/thread_pool_test.cpp")
    add_deps("ur")
    add_packages("gtest")

-- 定义性能测试程序
target("benchmark")
    set_kind("binary")
    add_files("tests/benchmark.cpp")
    add_deps("ur")
    add_packages("plf_nanotimer")


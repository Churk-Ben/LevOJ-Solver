add_rules("mode.debug", "mode.release")
add_requires("nlohmann_json", "cpp-httplib")

target("LevOJ-Solver")
    set_kind("binary")
    add_files("src/*.cpp")
    
    -- 链接依赖
    add_packages("nlohmann_json", "cpp-httplib")
    
    -- C++17 标准
    set_languages("cxx17")
    
    -- 可选：开启优化
    if is_mode("release") then
        set_optimize("fastest")
    end

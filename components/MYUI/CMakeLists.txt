# components/MYUI/CMakeLists.txt

# 注册本目录为一个 IDF 组件
idf_component_register(
    SRCS
        "ui.c"
        "ui_helpers.c"
        "screens/ui_Screen1.c"
        "components/ui_comp_hook.c"

    INCLUDE_DIRS
        "."
        "screens"
        "components"

    REQUIRES
        lvgl
)
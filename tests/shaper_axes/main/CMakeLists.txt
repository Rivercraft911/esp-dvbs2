idf_component_register(SRCS "../../shaper_axes.c"
    "../../../firmware/components/shaper/shaper.c" "../../../firmware/components/shaper/shaper_axes.c"
    "../../../firmware/components/shaper/shaper_pie.S" "../../../firmware/components/shaper/shaper_axes_pie.S"
    "../../../firmware/components/shaper/shaper_short_pie.S"
    LDFRAGMENTS "axes.lf"
    INCLUDE_DIRS "../../../firmware/components/shaper/include")

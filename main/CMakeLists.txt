idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	SRCS
		"main.c"
		"rom_selector.c"
		"menu.c"
	PRIV_REQUIRES
		esp_mm
		esp_lcd
		esp_driver_ppa
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		gpsp
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
# Prevent .sbss overflow: force small globals into regular .bss for all components
idf_build_set_property(COMPILE_OPTIONS "-msmall-data-limit=0" APPEND)

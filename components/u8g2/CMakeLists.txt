file(GLOB_RECURSE SRCS "u8g2/csrc/*.c")
idf_component_register(SRCS ${SRCS} INCLUDE_DIRS "u8g2/csrc")

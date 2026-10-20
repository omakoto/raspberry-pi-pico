# Platform-independent logic (BTHome decoding, triggers, the press flow, config validation). It has no
# ESP-IDF includes so the same sources are unit-tested on the PC by test/run.sh; cJSON is the only dependency.
idf_component_register(
    SRCS
        "src/models.cpp"
        "src/bthome.cpp"
        "src/triggers.cpp"
        "src/learn.cpp"
        "src/beeps.cpp"
        "src/eventlog.cpp"
        "src/config.cpp"
        "src/util.cpp"
        "src/auth.cpp"
        "src/app.cpp"
        "src/api.cpp"
        "src/switchbot.cpp"
        "src/switchbot_commands.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        json
)

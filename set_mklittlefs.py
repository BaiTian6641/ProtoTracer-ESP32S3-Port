Import("env")

# Force PlatformIO to use the project-local mklittlefs binary in the repo root.
# This overrides the default downloaded tool for LittleFS image creation.
env.Replace(MKLITTLEFS=str(env.Dir("$PROJECT_DIR").File("mklittlefs")))

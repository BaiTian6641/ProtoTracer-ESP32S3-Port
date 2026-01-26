Import("env")
# Ensure PlatformIO uses the project-local mklittlefs binary.
env.Replace(MKLITTLEFS=str(env.Dir("$PROJECT_DIR").File("mklittlefs")))
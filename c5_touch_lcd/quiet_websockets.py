# PlatformIO pre-script for the LCD build.
#
# WebSockets 2.6.1 (still in 2.7.x) calls NetworkClient::flush(), which
# Arduino-ESP32 3.x marks deprecated, so every build warned about it. The
# warning is silenced for that one library file only (this project does not
# use the WebSocket client); everything else keeps the usual warnings.
Import("env")


def quiet(env, node):
    return env.Object(node, CCFLAGS=env["CCFLAGS"] + ["-Wno-deprecated-declarations"])


env.AddBuildMiddleware(quiet, "*WebSocketsClient.cpp")

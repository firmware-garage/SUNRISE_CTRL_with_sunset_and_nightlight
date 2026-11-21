# PlatformIO pre-build script: warn loudly when the OTA password is still the
# placeholder, so a USB flash doesn't quietly put "changeme" on the board.
Import("env")

import os

pw = env.GetProjectConfig().get("secrets", "ota_password")
has_file = os.path.isfile(os.path.join(env.subst("$PROJECT_DIR"), "secrets.ini"))

if pw in ("changeme", "put-your-password-here", ""):
    print("")
    print("*** WARNING: OTA password is the placeholder '%s'. ***" % pw)
    if not has_file:
        print("*** Copy secrets.ini.example to secrets.ini and set ota_password. ***")
    print("")
if any(c in pw for c in '"\\ '):
    print("*** ERROR: ota_password can't contain quotes, backslashes or spaces. ***")
    env.Exit(1)

Import("env")  # type: ignore[name-defined]

# 出力するファームウェアのファイル名に地域を付ける (例: PicoCMT_JP.uf2)
# 地域は platformio.ini の各環境の custom_fw_region で指定する
region = env.GetProjectOption("custom_fw_region", "")  # type: ignore[name-defined]
env.Replace(PROGNAME="PicoCMT_%s" % region if region else "PicoCMT")  # type: ignore[name-defined]

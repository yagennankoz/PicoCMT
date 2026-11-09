#pragma once

#include <stdint.h>

// ============================================================================
// SDカードをUSBドライブ(マスストレージ)としてPCに見せる
//   USBドライブの間は、PicoCmtの他の機能(再生・録音・ファイル一覧・Wi-Fi)を止め、
//   SDカードはPCだけが読み書きする(同時に書くとFATが壊れるため)
// ============================================================================

// setup()の最初で呼ぶ(USBの機能にマスストレージを登録する。SDはまだ見せない)
void usb_storage_init();

// PCとUSBでつながっているか(PCに認識されている)
bool usb_host_connected();

// SDカードをUSBドライブにする(SDライブラリは止める)。SDが入っていない、PCとつながっていないときは
// falseを返す。その場合もSDライブラリは止まっているので、SDを読み直すこと
bool usb_storage_start();

// USBドライブをやめる(PCからはSDが抜かれたように見える)。この後SDを読み直すこと
void usb_storage_stop();

bool usb_storage_active();

// PCで「取り出し」をした
bool usb_storage_eject_requested();

// PCが読み書きした量(KB)。画面の表示用
uint32_t usb_storage_read_kb();
uint32_t usb_storage_write_kb();

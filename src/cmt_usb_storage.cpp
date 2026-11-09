#include "cmt_usb_storage.h"

#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "cmt_pins.h"

// USBのマスストレージ
static Adafruit_USBD_MSC usb_msc;

// USBドライブとして見せている間だけtrue(読み書きの呼び出しは、USBの処理の割り込みから来る)
static volatile bool msc_active = false;
static volatile bool msc_eject_requested = false;
static volatile uint32_t msc_read_sectors = 0;
static volatile uint32_t msc_write_sectors = 0;

// USBドライブの間はSDライブラリを止め、同じSPI・CSでカードのセクタを直接読み書きする
static SdSpiCard sd_card;
static bool sd_card_ready = false;

static SdSpiCard* sd_card_device() { return sd_card_ready ? &sd_card : nullptr; }

// PCがセクタを読む
static int32_t msc_read_cb(uint32_t lba, void* buffer, uint32_t bufsize) {
    SdSpiCard* card = sd_card_device();
    if (!msc_active || card == nullptr) {
        return -1;
    }
    uint32_t n = bufsize / 512;
    if (!card->readSectors(lba, (uint8_t*)buffer, n)) {
        return -1;
    }
    msc_read_sectors += n;
    return (int32_t)bufsize;
}

// PCがセクタを書く
static int32_t msc_write_cb(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
    SdSpiCard* card = sd_card_device();
    if (!msc_active || card == nullptr) {
        return -1;
    }
    uint32_t n = bufsize / 512;
    if (!card->writeSectors(lba, buffer, n)) {
        return -1;
    }
    msc_write_sectors += n;
    return (int32_t)bufsize;
}

// PCの1回の書き込み(複数のセクタ)が終わった。続けて書いていたのを閉じる
static void msc_flush_cb() {
    SdSpiCard* card = sd_card_device();
    if (msc_active && card != nullptr) {
        card->syncDevice();
    }
}

// PCで「取り出し」をすると、止める(start=false)と取り出す(load_eject=true)が来る
static bool msc_start_stop_cb(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject && !start) {
        msc_eject_requested = true;
    }
    return true;
}

void usb_storage_init() {
    usb_msc.setID("PicoCmt", "SD Card", "1.0");
    usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
    usb_msc.setStartStopCallback(msc_start_stop_cb);
    // 最初はSDが入っていないドライブとして見せる(USBドライブにしたときだけ読み書きできる)
    usb_msc.setUnitReady(false);
    usb_msc.begin();

    // すでにPCに認識されていたら、マスストレージを足して認識し直してもらう
    if (TinyUSBDevice.mounted()) {
        TinyUSBDevice.detach();
        delay(10);
        TinyUSBDevice.attach();
    }
}

bool usb_host_connected() { return TinyUSBDevice.mounted(); }

bool usb_storage_start() {
    if (msc_active || !usb_host_connected()) {
        return false;
    }
    // SDライブラリを止めて、カードを直接開く。USBは1回に1セクタずつ読み書きを頼んでくるので、
    // SPIを専有するモードにして、続いたセクタはカードへの1つの読み書きの続きとして送る
    // (セクタごとにカードに頼むと、特に書き込みが遅い)。書き込みはPCの1回の書き込みが終わったら閉じる
    SD.end();
    sd_card_ready = sd_card.begin(SdSpiConfig(PICO_SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(16), &SPI1));
    uint32_t sectors = sd_card_ready ? sd_card.sectorCount() : 0;
    if (sectors == 0) {
        sd_card_ready = false;
        return false;
    }
    msc_eject_requested = false;
    msc_read_sectors = 0;
    msc_write_sectors = 0;
    usb_msc.setCapacity(sectors, 512);
    msc_active = true;
    usb_msc.setUnitReady(true);
    return true;
}

void usb_storage_stop() {
    if (!msc_active) {
        return;
    }
    // 先に読み書きを止めてから、PCにSDが抜かれたことを知らせる
    msc_active = false;
    usb_msc.setUnitReady(false);
    if (sd_card_ready) {
        sd_card.syncDevice();
        sd_card_ready = false;
    }
}

bool usb_storage_active() { return msc_active; }

bool usb_storage_eject_requested() { return msc_eject_requested; }

uint32_t usb_storage_read_kb() { return msc_read_sectors / 2; }

uint32_t usb_storage_write_kb() { return msc_write_sectors / 2; }

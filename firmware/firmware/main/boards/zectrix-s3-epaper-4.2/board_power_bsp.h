#ifndef __BOARD_POWER_BSP_H__
#define __BOARD_POWER_BSP_H__

#include <atomic>

class ChargeStatus;

/**
 * @brief Power rails this board owns. The speaker amplifier is not one of them.
 *
 * GPIO46 used to be here as well, as `Audio_AMP_PIN`, alongside
 * `AUDIO_CODEC_PA_PIN` in `Es8311AudioCodec`. Two owners for one pin was safe
 * only for as long as neither of them actually drove it: `PowerAmpOn/Off` had
 * no callers, and nothing ever enabled codec output.
 *
 * Push-to-talk changes both halves of that. The codec now holds GPIO46 with
 * `gpio_hold_en` every time output is enabled for an earcon, and a later call
 * to `PowerAmpOff` would silence the amplifier behind the codec's back with no
 * way for the codec to notice. So the pin has one owner: the codec. The
 * constructor no longer configures it and the two amplifier methods are gone
 * rather than left as traps.
 *
 * This is a code decision, not a hardware finding. Whether the polarity is
 * right on the physical board is HG5.3 in docs/HARDWARE-ACCEPTANCE.md and has
 * never been checked.
 */
class BoardPowerBsp {
private:
    const int epdPowerPin_;
    const int audioPowerPin_;
    const int vbatPowerPin_;
    ChargeStatus* charge_status_ = nullptr;
    std::atomic<bool> led_override_enabled_{false};
    std::atomic<bool> led_override_blink_{false};
    std::atomic<bool> led_override_phase_{false};
    std::atomic<int> led_activity_pulses_{0};

    static void PowerLedTask(void *arg);

public:
    BoardPowerBsp(int epdPowerPin, int audioPowerPin, int vbatPowerPin,
                  ChargeStatus* charge_status);
    ~BoardPowerBsp();
    void PowerEpdOn();
    void PowerEpdOff();
    void PowerAudioOn();
    void PowerAudioOff();
    void VbatPowerOn();
    void VbatPowerOff();
    void SetFactoryLedOverride(bool enabled, bool blink);
    void FlashActivityLed();
};

#endif

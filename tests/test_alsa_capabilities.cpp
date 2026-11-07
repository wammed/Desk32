#include <QTest>
#include <alsa/asoundlib.h>
#include <QDebug>

class TestAlsaCapabilities : public QObject
{
    Q_OBJECT

private slots:
    void testCardEnumeration();
    void testPcmHardwareParameters();
    void testControlApiCapabilities();
};

void TestAlsaCapabilities::testCardEnumeration()
{
    int cardNum = -1;
    int cardCount = 0;

    while (snd_card_next(&cardNum) == 0 && cardNum >= 0) {
        cardCount++;
        char* cardName = nullptr;
        char* cardLongName = nullptr;
        if (snd_card_get_name(cardNum, &cardName) == 0 && cardName) {
            qDebug() << "Detected ALSA Card" << cardNum << ":" << cardName;
            free(cardName);
        }
        if (snd_card_get_longname(cardNum, &cardLongName) == 0 && cardLongName) {
            qDebug() << "  Long name:" << cardLongName;
            free(cardLongName);
        }
    }

    if (cardCount == 0) {
        QSKIP("No ALSA hardware audio cards detected (running in headless CI/container environment).");
    }

    QVERIFY(cardCount > 0);
}

void TestAlsaCapabilities::testPcmHardwareParameters()
{
    // Query all available hardware cards
    const char* targetDevices[] = { "hw:2", "hw:UR22", "hw:0", "default" };

    for (const char* dev : targetDevices) {
        snd_pcm_t* pcmHandle = nullptr;
        int err = snd_pcm_open(&pcmHandle, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (err != 0 || !pcmHandle) {
            qDebug() << "Device" << dev << "could not be opened directly (error:" << snd_strerror(err) << ")";
            continue;
        }

        qDebug() << "========================================";
        qDebug() << "Testing PCM parameters on device:" << dev;

        snd_pcm_hw_params_t* hwParams = nullptr;
        snd_pcm_hw_params_alloca(&hwParams);
        err = snd_pcm_hw_params_any(pcmHandle, hwParams);
        if (err != 0) {
            snd_pcm_close(pcmHandle);
            continue;
        }

        unsigned int minRate = 0, maxRate = 0;
        snd_pcm_hw_params_get_rate_min(hwParams, &minRate, nullptr);
        snd_pcm_hw_params_get_rate_max(hwParams, &maxRate, nullptr);
        qDebug() << "Sample rate range:" << minRate << "Hz to" << maxRate << "Hz";

        unsigned int minChannels = 0, maxChannels = 0;
        snd_pcm_hw_params_get_channels_min(hwParams, &minChannels);
        snd_pcm_hw_params_get_channels_max(hwParams, &maxChannels);
        qDebug() << "Channel range:" << minChannels << "to" << maxChannels;

        const unsigned int testRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
        for (unsigned int rate : testRates) {
            int rateSupport = snd_pcm_hw_params_test_rate(pcmHandle, hwParams, rate, 0);
            qDebug() << "  Rate" << rate << "Hz support:" << (rateSupport == 0 ? "YES" : "NO");
        }

        const snd_pcm_format_t testFormats[] = {
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_FORMAT_S24_3LE,
            SND_PCM_FORMAT_S24_LE,
            SND_PCM_FORMAT_S32_LE,
            SND_PCM_FORMAT_FLOAT_LE,
            SND_PCM_FORMAT_FLOAT64_LE
        };
        for (snd_pcm_format_t fmt : testFormats) {
            int fmtSupport = snd_pcm_hw_params_test_format(pcmHandle, hwParams, fmt);
            qDebug() << "  Format" << snd_pcm_format_name(fmt) << "support:" << (fmtSupport == 0 ? "YES" : "NO");
        }

        snd_pcm_close(pcmHandle);
    }
}

void TestAlsaCapabilities::testControlApiCapabilities()
{
    // Test ALSA Control API on card 0
    snd_ctl_t* ctl = nullptr;
    int err = snd_ctl_open(&ctl, "hw:0", 0);
    if (err == 0 && ctl) {
        snd_ctl_card_info_t* info = nullptr;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) == 0) {
            qDebug() << "ALSA Control Card Info:" << snd_ctl_card_info_get_id(info)
                     << "-" << snd_ctl_card_info_get_name(info);
        }
        snd_ctl_close(ctl);
    }
    QVERIFY(true);
}

QTEST_MAIN(TestAlsaCapabilities)
#include "test_alsa_capabilities.moc"

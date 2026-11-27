#include <QtTest/QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include "core/EngineController.hpp"
#include "core/SettingsManager.hpp"
#include "core/log/LogService.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/preset/BuiltInDspPresets.hpp"
#include "engine/preset/DspPresetSerializer.hpp"
#include "engine/preset/PresetManager.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/workspace/WorkspaceManager.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace desk32::core;
using namespace desk32::engine::preset;

namespace {

/// Measure effective RT60 (seconds) via Schroeder backward-integration (T30: -5 dB to -35 dB extrapolated x2)
double measureReverbRt60(double sampleRate, double size, double decayS, double damping) {
    AlgorithmicReverb rev;
    rev.configure(sampleRate, size, damping, 1.0, 0.0, true, true, decayS);

    const size_t totalFrames = static_cast<size_t>(std::ceil(std::max(1.5, decayS * 2.2) * sampleRate));
    std::vector<double> irSq(totalFrames, 0.0);
    for (size_t i = 0; i < totalFrames; ++i) {
        double l = (i == 0) ? 1.0 : 0.0;
        double r = (i == 0) ? 1.0 : 0.0;
        rev.process(l, r);
        irSq[i] = 0.5 * (l * l + r * r);
    }

    // Backward cumulative energy integration (Schroeder curve)
    std::vector<double> schroeder(totalFrames, 0.0);
    double acc = 0.0;
    for (size_t k = totalFrames; k > 0; --k) {
        acc += irSq[k - 1];
        schroeder[k - 1] = acc;
    }
    if (schroeder[0] <= 1e-18) return 0.0;

    size_t idx5 = 0;
    size_t idx35 = 0;
    bool found5 = false;
    bool found35 = false;
    const double e0 = schroeder[0];
    for (size_t i = 0; i < totalFrames; ++i) {
        const double db = 10.0 * std::log10(std::max(1e-24, schroeder[i] / e0));
        if (!found5 && db <= -5.0) {
            idx5 = i;
            found5 = true;
        }
        if (found5 && !found35 && db <= -35.0) {
            idx35 = i;
            found35 = true;
            break;
        }
    }
    if (!found5 || !found35 || idx35 <= idx5) return 0.0;
    const double t30Sec = static_cast<double>(idx35 - idx5) / sampleRate;
    return t30Sec * 2.0;
}

} // namespace

class TestDspPresets : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // P1: Data model & JSON validation
    void testP1RoundTripAllDspAndGlobal();
    void testP1ClampingDefaultsOverflowUnknownsAndVersionRejection();
    void testP1UserPresetPersistenceOverwriteRenameAndIsolation();
    void testP1LegacyV1Migration();

    // P4: DSP additions (18 dB/oct HPF/LPF & Reverb Decay / Pre-delay 200 ms)
    void testP4Filter18DbPerOctaveResponse();
    void testP4ReverbDecayConversionRt60Measurement();
    void testP4ReverbPredelay200Ms();

    // P2: All 48 Built-in Presets exhaustive verification
    void testP2All48BuiltInPresetsExhaustive();

    // P3: Atomic Global & Per-DSP application, exclusions, failure preservation, Render sync
    void testP3GlobalAndPerDspApplicationAndExclusions();
    void testP3InvalidFilePreservesPriorState();
    void testP3RenderPathReflectsGlobalPreset();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestDspPresets::initTestCase() {
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    desk32::core::log::LogService::instance().start(m_tempDir->filePath(QStringLiteral("test_dsp_presets_log.db")));
    PresetManager::instance().setCustomRootPath(m_tempDir->filePath(QStringLiteral("presets")));
    desk32::engine::workspace::WorkspaceManager::instance().setConfigFilePathForTesting(
        m_tempDir->filePath(QStringLiteral("workspaces.json")));
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
}

void TestDspPresets::cleanupTestCase() {
    desk32::engine::render::RenderEngine::instance().stop();
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
    desk32::engine::render::RenderSequenceManager::instance().close();
    desk32::core::log::LogService::instance().stop();
    SettingsManager::instance().resetForTesting();
    m_tempDir.reset();
}

void TestDspPresets::testP1RoundTripAllDspAndGlobal() {
    // Test round-trip for Global and all 7 Per-DSP kinds
    {
        DspPresetDocument orig = BuiltInDspPresets::allPresets().at(1); // Vocal Forward
        orig.isFactory = false;
        const QByteArray bytes = DspPresetSerializer::toJsonBytes(orig);

        DspPresetDocument parsed;
        PresetValidationReport rep;
        QVERIFY2(DspPresetSerializer::fromJsonBytes(bytes, parsed, rep),
                 qPrintable(rep.errorMessage));
        QVERIFY(rep.ok);
        QVERIFY(!rep.hasWarnings());
        QCOMPARE(parsed.kind, DspPresetKind::Global);
        QCOMPARE(parsed.id, orig.id);
        QCOMPARE(parsed.name, orig.name);
        QCOMPARE(parsed.scope, DspPresetScope::Chain);
        QVERIFY(DspPresetSerializer::isGlobalStateEqual(orig.global, parsed.global, 1e-6));
    }

    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        const auto target = static_cast<DspPresetTarget>(i);
        const auto list = BuiltInDspPresets::presetsFor(DspPresetKind::PerDsp, target);
        QVERIFY(list.size() >= 2);
        const DspPresetDocument& orig = list[1]; // Non-unity preset
        const QByteArray bytes = DspPresetSerializer::toJsonBytes(orig);

        DspPresetDocument parsed;
        PresetValidationReport rep;
        QVERIFY2(DspPresetSerializer::fromJsonBytes(bytes, parsed, rep),
                 qPrintable(rep.errorMessage));
        QVERIFY(rep.ok);
        QVERIFY(!rep.hasWarnings());
        QCOMPARE(parsed.kind, DspPresetKind::PerDsp);
        QCOMPARE(parsed.target, target);
        QCOMPARE(parsed.scope, target == DspPresetTarget::Peq ? DspPresetScope::MasteringChain : DspPresetScope::Chain);
        QVERIFY(DspPresetSerializer::isGlobalStateEqual(orig.global, parsed.global, 1e-6));
    }
}

void TestDspPresets::testP1ClampingDefaultsOverflowUnknownsAndVersionRejection() {
    // 1. Out-of-range clamp + unknown key preservation + unknown enum fallback + PEQ >16 overflow + legacy created/modified
    QJsonObject peqObj;
    peqObj.insert(QStringLiteral("preamp_db"), 99.0); // clamp -> +24
    peqObj.insert(QStringLiteral("custom_vendor_meta"), QStringLiteral("keep_me"));
    QJsonArray leftBands;
    for (int i = 0; i < 18; ++i) {
        QJsonObject b;
        b.insert(QStringLiteral("freq_hz"), i == 0 ? 5.0 : 1000.0); // clamp -> 10.0
        b.insert(QStringLiteral("gain_db"), i == 0 ? -50.0 : 1.0);  // clamp -> -24.0
        b.insert(QStringLiteral("q"), i == 0 ? 100.0 : 1.0);        // clamp -> 20.0
        b.insert(QStringLiteral("type"), i == 1 ? QStringLiteral("mystery_filter") : QStringLiteral("peak"));
        b.insert(QStringLiteral("enabled"), true);
        leftBands.append(b);
    }
    QJsonObject leftSide;
    leftSide.insert(QStringLiteral("bands"), leftBands);
    peqObj.insert(QStringLiteral("left"), leftSide);
    // Intentionally omit `right` to test missing-field Unity default補完

    QJsonObject docObj;
    docObj.insert(QStringLiteral("format"), QString::fromLatin1(kDspPresetFormat));
    docObj.insert(QStringLiteral("version"), kDspPresetCurrentVersion);
    docObj.insert(QStringLiteral("kind"), QStringLiteral("per-dsp"));
    docObj.insert(QStringLiteral("id"), QStringLiteral("test.peq.edge"));
    docObj.insert(QStringLiteral("name"), QStringLiteral("Edge PEQ"));
    docObj.insert(QStringLiteral("scope"), QStringLiteral("mastering-chain"));
    docObj.insert(QStringLiteral("target"), QStringLiteral("peq"));
    docObj.insert(QStringLiteral("created"), QStringLiteral("2026-11-27T01:00:00Z"));
    docObj.insert(QStringLiteral("modified"), QStringLiteral("2026-11-27T02:00:00Z"));
    docObj.insert(QStringLiteral("top_unknown"), 42);
    docObj.insert(QStringLiteral("dsp"), peqObj);

    DspPresetDocument parsed;
    PresetValidationReport rep;
    QVERIFY(DspPresetSerializer::fromJson(docObj, parsed, rep));
    QVERIFY(rep.ok);
    QCOMPARE(parsed.global.peq.preampDb, 24.0);
    QCOMPARE(parsed.global.peq.leftBands[0].freqHz, 10.0);
    QCOMPARE(parsed.global.peq.leftBands[0].gainDb, -24.0);
    QCOMPARE(parsed.global.peq.leftBands[0].q, 20.0);
    QCOMPARE(parsed.global.peq.leftBands[1].type, BiquadFilterType::Peak);
    QCOMPARE(parsed.global.peq.overflowLeft.size(), size_t(2));
    QCOMPARE(rep.peqOverflowLeft, size_t(2));
    QVERIFY(!rep.clampedFields.isEmpty());
    QVERIFY(!rep.unknownEnumFields.isEmpty());
    QVERIFY(!rep.unknownKeys.isEmpty());
    QVERIFY(!rep.missingDefaultedFields.isEmpty());
    QCOMPARE(parsed.createdAt, QStringLiteral("2026-11-27T01:00:00Z"));
    QCOMPARE(parsed.modifiedAt, QStringLiteral("2026-11-27T02:00:00Z"));

    // Verify unknown keys & overflow survive re-serialization
    const QJsonObject reOut = DspPresetSerializer::toJson(parsed);
    QCOMPARE(reOut.value(QStringLiteral("top_unknown")).toInt(), 42);
    QCOMPARE(reOut.value(QStringLiteral("dsp")).toObject().value(QStringLiteral("custom_vendor_meta")).toString(),
             QStringLiteral("keep_me"));
    QCOMPARE(reOut.value(QStringLiteral("dsp")).toObject()
                 .value(QStringLiteral("left")).toObject()
                 .value(QStringLiteral("bands")).toArray().size(),
             18);
    QVERIFY(reOut.contains(QStringLiteral("created_at")));
    QVERIFY(reOut.contains(QStringLiteral("modified_at")));

    // 2. Unknown HPF slope enum falls back to 12 dB/oct + reports
    QJsonObject hpfDoc;
    hpfDoc.insert(QStringLiteral("format"), QString::fromLatin1(kDspPresetFormat));
    hpfDoc.insert(QStringLiteral("version"), kDspPresetCurrentVersion);
    hpfDoc.insert(QStringLiteral("kind"), QStringLiteral("per-dsp"));
    hpfDoc.insert(QStringLiteral("id"), QStringLiteral("test.hpf.badslope"));
    hpfDoc.insert(QStringLiteral("name"), QStringLiteral("Bad Slope"));
    hpfDoc.insert(QStringLiteral("scope"), QStringLiteral("chain"));
    hpfDoc.insert(QStringLiteral("target"), QStringLiteral("hpf"));
    QJsonObject hpfDsp;
    hpfDsp.insert(QStringLiteral("cutoff_hz"), 80.0);
    hpfDsp.insert(QStringLiteral("q"), 0.8);
    hpfDsp.insert(QStringLiteral("slope_db_oct"), 36); // invalid
    hpfDoc.insert(QStringLiteral("dsp"), hpfDsp);
    DspPresetDocument hpfParsed;
    PresetValidationReport hpfRep;
    QVERIFY(DspPresetSerializer::fromJson(hpfDoc, hpfParsed, hpfRep));
    QCOMPARE(hpfParsed.global.hpf.slopeDbOct, 12);
    QVERIFY(!hpfRep.unknownEnumFields.isEmpty());

    // 3. Unsupported future version rejected
    QJsonObject badVer = docObj;
    badVer.insert(QStringLiteral("version"), 99);
    DspPresetDocument badParsed;
    PresetValidationReport badRep;
    QVERIFY(!DspPresetSerializer::fromJson(badVer, badParsed, badRep));
    QVERIFY(!badRep.ok);
    QVERIFY(!badRep.errorMessage.isEmpty());

    // 4. Listening Correction scope rejected for Mastering Chain presets (§11.1)
    QJsonObject lcScope = docObj;
    lcScope.insert(QStringLiteral("scope"), QStringLiteral("listening-correction"));
    QVERIFY(!DspPresetSerializer::fromJson(lcScope, badParsed, badRep));
    QVERIFY(!badRep.ok);
}

void TestDspPresets::testP1UserPresetPersistenceOverwriteRenameAndIsolation() {
    auto& pm = PresetManager::instance();

    // Save new Global user preset
    DspPresetDocument doc;
    doc.kind = DspPresetKind::Global;
    doc.scope = DspPresetScope::Chain;
    doc.name = QStringLiteral("My Studio Master");
    doc.global = GlobalDspPresetData::unity();
    doc.global.setNodeEnabled(DspPresetTarget::Hpf, true);
    doc.global.hpf = {55.0, 0.75, 18, {}};

    QString err;
    QVERIFY2(pm.saveUserDspPreset(doc, false, &err), qPrintable(err));
    const QString savedId = doc.id;
    QVERIFY(!savedId.isEmpty());

    // Refuses silent overwrite when overwrite=false
    QVERIFY(!pm.saveUserDspPreset(doc, false, &err));
    QVERIFY(pm.saveUserDspPreset(doc, true, &err));

    // Refuses overwriting built-in preset
    DspPresetDocument builtInAttempt = doc;
    builtInAttempt.id.clear();
    builtInAttempt.name = QStringLiteral("Unity");
    QVERIFY(!pm.saveUserDspPreset(builtInAttempt, true, &err));
    QVERIFY(!pm.deleteUserDspPreset(DspPresetKind::Global, DspPresetTarget::Peq, QStringLiteral("builtin.global.unity"), &err));

    // Rename preserves immutable `id`
    QVERIFY2(pm.renameUserDspPreset(DspPresetKind::Global, DspPresetTarget::Peq,
                                    savedId, QStringLiteral("My Studio Master v2"), false, &err),
             qPrintable(err));
    DspPresetDocument loaded;
    PresetValidationReport rep;
    QVERIFY(pm.loadDspPreset(DspPresetKind::Global, DspPresetTarget::Peq, savedId, loaded, rep));
    QCOMPARE(loaded.id, savedId);
    QCOMPARE(loaded.name, QStringLiteral("My Studio Master v2"));
    QCOMPARE(loaded.global.hpf.slopeDbOct, 18);

    // Listening Correction files live in a completely separate directory from PEQ
    QVERIFY(pm.dspPresetDirectory(DspPresetKind::PerDsp, DspPresetTarget::Peq) !=
            pm.categoryDirectory(QString::fromLatin1(PresetCategories::ListeningCorrection)));

    // Clean delete
    QVERIFY(pm.deleteUserDspPreset(DspPresetKind::Global, DspPresetTarget::Peq, savedId, &err));
}

void TestDspPresets::testP1LegacyV1Migration() {
    QJsonObject v1;
    v1.insert(QStringLiteral("category"), QStringLiteral("master_dsp"));
    v1.insert(QStringLiteral("name"), QStringLiteral("Legacy Warm"));
    v1.insert(QStringLiteral("format_version"), 1);
    QJsonObject comp;
    comp.insert(QStringLiteral("enabled"), true);
    comp.insert(QStringLiteral("threshold_db"), -16.0);
    comp.insert(QStringLiteral("ratio"), 2.5);
    v1.insert(QStringLiteral("compressor"), comp);
    QJsonObject lim;
    lim.insert(QStringLiteral("enabled"), true);
    lim.insert(QStringLiteral("ceiling_db"), -0.5);
    lim.insert(QStringLiteral("release_ms"), 60.0);
    v1.insert(QStringLiteral("limiter"), lim);

    DspPresetDocument parsed;
    PresetValidationReport rep;
    QVERIFY(DspPresetSerializer::fromJson(v1, parsed, rep));
    QVERIFY(rep.ok);
    QVERIFY(rep.migratedFromLegacy);
    QCOMPARE(parsed.version, kDspPresetCurrentVersion);
    QVERIFY(parsed.global.isNodeEnabled(DspPresetTarget::Compressor));
    QCOMPARE(parsed.global.compressor.thresholdDb, -16.0);
    QCOMPARE(parsed.global.limiter.ceilingDbfs, -0.5);
}

void TestDspPresets::testP4Filter18DbPerOctaveResponse() {
    // Verify 18 dB/oct HPF (Vocal Cut: 120 Hz, Q=0.80) and LPF (Tape Soft: 14 kHz, Q=0.85)
    constexpr double sr = 48000.0;

    HighLowPassFilter hpf12, hpf18, hpf24;
    hpf12.configure(sr, 200.0, 20000.0, HighLowPassFilter::FilterSlope::Slope12dB, HighLowPassFilter::FilterSlope::Slope12dB, 0.71, 0.71);
    hpf18.configure(sr, 200.0, 20000.0, HighLowPassFilter::FilterSlope::Slope18dB, HighLowPassFilter::FilterSlope::Slope12dB, 0.71, 0.71);
    hpf24.configure(sr, 200.0, 20000.0, HighLowPassFilter::FilterSlope::Slope24dB, HighLowPassFilter::FilterSlope::Slope12dB, 0.71, 0.71);
    hpf12.setHpfEnabled(true); hpf12.setLpfEnabled(false);
    hpf18.setHpfEnabled(true); hpf18.setLpfEnabled(false);
    hpf24.setHpfEnabled(true); hpf24.setLpfEnabled(false);

    // Measure stopband slope between 50 Hz and 25 Hz (1 octave well below 200 Hz cutoff)
    const std::vector<double> freqs = {25.0, 50.0, 200.0, 2000.0};
    std::vector<double> r12, r18, r24;
    hpf12.calculateResponseCurve(freqs, r12);
    hpf18.calculateResponseCurve(freqs, r18);
    hpf24.calculateResponseCurve(freqs, r24);

    const double slope12 = r12[1] - r12[0]; // ~12 dB/oct
    const double slope18 = r18[1] - r18[0]; // ~18 dB/oct
    const double slope24 = r24[1] - r24[0]; // ~24 dB/oct

    qInfo("HPF measured stopband slopes (50Hz -> 25Hz): 12dB=%.2f, 18dB=%.2f, 24dB=%.2f dB/oct",
          slope12, slope18, slope24);
    QVERIFY2(std::abs(slope12 - 12.0) < 0.5, qPrintable(QString::number(slope12)));
    QVERIFY2(std::abs(slope18 - 18.0) < 0.5, qPrintable(QString::number(slope18)));
    QVERIFY2(std::abs(slope24 - 24.0) < 0.5, qPrintable(QString::number(slope24)));
    QVERIFY(std::abs(r18[3]) < 0.2); // Passband unity at 2 kHz

    // Also verify LPF 18 dB/oct stopband slope between 4 kHz and 8 kHz (cutoff = 1000 Hz)
    HighLowPassFilter lpf18;
    lpf18.configure(sr, 20.0, 1000.0, HighLowPassFilter::FilterSlope::Slope12dB, HighLowPassFilter::FilterSlope::Slope18dB, 0.71, 0.85);
    lpf18.setHpfEnabled(false);
    lpf18.setLpfEnabled(true);
    const std::vector<double> lpfFreqs = {100.0, 4000.0, 8000.0};
    std::vector<double> lr18;
    lpf18.calculateResponseCurve(lpfFreqs, lr18);
    const double lpfSlope18 = lr18[1] - lr18[2];
    qInfo("LPF 18dB/oct measured stopband slope (4kHz -> 8kHz): %.2f dB/oct", lpfSlope18);
    QVERIFY2(lpfSlope18 > 17.0 && lpfSlope18 < 21.0, qPrintable(QString::number(lpfSlope18)));
    QVERIFY(std::abs(lr18[0]) < 0.2); // Passband unity at 100 Hz
}

void TestDspPresets::testP4ReverbDecayConversionRt60Measurement() {
    struct Case {
        const char* name;
        double size;
        double decayS;
        double damping;
    };
    const Case cases[] = {
        {"Small Room", 0.25, 0.6, 0.60},
        {"Studio",     0.45, 1.1, 0.50},
        {"Hall",       0.85, 2.5, 0.40},
        {"Cathedral",  0.95, 4.2, 0.35},
    };

    for (const auto& c : cases) {
        const double measured = measureReverbRt60(48000.0, c.size, c.decayS, c.damping);
        const double relErr = std::abs(measured - c.decayS) / c.decayS;
        qInfo("Reverb RT60 [%s]: target=%.2fs, measured=%.3fs, relErr=%.1f%%",
              c.name, c.decayS, measured, relErr * 100.0);
        QVERIFY2(relErr <= 0.20,
                 qPrintable(QString("Reverb '%1' RT60 out of +/-20%% tolerance: target=%2, measured=%3")
                                .arg(c.name).arg(c.decayS).arg(measured)));
    }
}

void TestDspPresets::testP4ReverbPredelay200Ms() {
    // Verify 200 ms pre-delay at both 48 kHz and 192 kHz (not clamped to 100 ms)
    for (double sr : {48000.0, 192000.0}) {
        AlgorithmicReverb rev;
        rev.configure(sr, 0.5, 0.3, 1.0, 200.0, true, true, 1.0);
        QCOMPARE(rev.predelayMs(), 200.0);

        const size_t expectedDelay = static_cast<size_t>(std::ceil(sr * 0.200));
        size_t firstNonZero = 0;
        bool found = false;
        for (size_t i = 0; i < expectedDelay + 4096; ++i) {
            double l = (i == 0) ? 1.0 : 0.0;
            double r = (i == 0) ? 1.0 : 0.0;
            rev.process(l, r);
            if (!found && (std::abs(l) > 1e-6 || std::abs(r) > 1e-6)) {
                firstNonZero = i;
                found = true;
                break;
            }
        }
        QVERIFY(found);
        // Impulse arrives after pre-delay + shortest comb delay
        QVERIFY2(firstNonZero >= expectedDelay,
                 qPrintable(QString("sr=%1: firstNonZero=%2 < expectedDelay=%3")
                                .arg(sr).arg(firstNonZero).arg(expectedDelay)));
    }
}

void TestDspPresets::testP2All48BuiltInPresetsExhaustive() {
    const auto& all = BuiltInDspPresets::allPresets();
    QCOMPARE(all.size(), size_t(48));

    QSet<QString> seenIds;
    size_t globalCount = 0;
    std::array<size_t, kDspPresetTargetCount> perDspCounts{};

    for (const auto& doc : all) {
        QVERIFY2(!seenIds.contains(doc.id), qPrintable(QString("Duplicate id: %1").arg(doc.id)));
        seenIds.insert(doc.id);
        QVERIFY(doc.isFactory);

        // Strict in-range check per Appendix A.3-A.9
        QStringList violations;
        QVERIFY2(DspPresetSerializer::validateStrictInRange(doc, violations),
                 qPrintable(QString("%1 violations: %2").arg(doc.id, violations.join("; "))));

        // Zero-warning JSON round-trip check
        const QByteArray json = DspPresetSerializer::toJsonBytes(doc);
        DspPresetDocument parsed;
        PresetValidationReport rep;
        QVERIFY2(DspPresetSerializer::fromJsonBytes(json, parsed, rep),
                 qPrintable(QString("%1 parse error: %2").arg(doc.id, rep.errorMessage)));
        QVERIFY2(!rep.hasWarnings(),
                 qPrintable(QString("%1 warnings: %2").arg(doc.id, rep.warnings.join("; "))));
        QVERIFY(DspPresetSerializer::isGlobalStateEqual(doc.global, parsed.global, 1e-6));

        if (doc.kind == DspPresetKind::Global) {
            ++globalCount;
        } else {
            ++perDspCounts[static_cast<size_t>(doc.target)];
        }
    }

    QCOMPARE(globalCount, size_t(6));
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QCOMPARE(perDspCounts[i], size_t(6));
    }

    // Verify Unity presets strictly match Appendix A.3-A.9 defaults
    const auto gUnity = BuiltInDspPresets::globalUnity();
    QVERIFY(DspPresetSerializer::isGlobalStateEqual(gUnity.global, GlobalDspPresetData::unity(), 1e-9));
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QVERIFY(!gUnity.global.nodeEnabled[i]);
    }

    // Verify `BASS & TREBLE BOOST` name on GEQ and PEQ, and 18 dB/oct presets exist
    DspPresetDocument tmp;
    QVERIFY(BuiltInDspPresets::findPreset(DspPresetKind::PerDsp, DspPresetTarget::Geq,
                                          QStringLiteral("BASS & TREBLE BOOST"), tmp));
    QVERIFY(BuiltInDspPresets::findPreset(DspPresetKind::PerDsp, DspPresetTarget::Peq,
                                          QStringLiteral("BASS & TREBLE BOOST"), tmp));
    QVERIFY(BuiltInDspPresets::findPreset(DspPresetKind::PerDsp, DspPresetTarget::Hpf,
                                          QStringLiteral("Vocal Cut"), tmp));
    QCOMPARE(tmp.global.hpf.slopeDbOct, 18);
    QVERIFY(BuiltInDspPresets::findPreset(DspPresetKind::PerDsp, DspPresetTarget::Lpf,
                                          QStringLiteral("Tape Soft"), tmp));
    QCOMPARE(tmp.global.lpf.slopeDbOct, 18);
}

void TestDspPresets::testP3GlobalAndPerDspApplicationAndExclusions() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    // Set non-default excluded parameters before applying presets
    ctrl.setDspTrim(QStringLiteral("peq"), false, 2.5);
    ctrl.setDspTrim(QStringLiteral("compressor"), true, -3.0);
    ctrl.setMasterGain(0.75);
    ctrl.setGeqGlobalBypass(true);
    ctrl.setListeningPreset(QStringLiteral("Harman Target"));

    // 1. Apply Global "Vocal Forward"
    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Vocal Forward")));
    QVERIFY(!ctrl.isDspNodeBypassed(QStringLiteral("PEQ")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("GEQ")));
    QVERIFY(!ctrl.isDspNodeBypassed(QStringLiteral("Compressor")));
    QVERIFY(!ctrl.isDspNodeBypassed(QStringLiteral("Limiter")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Reverb")));

    const QVariantMap flt = ctrl.getFilterParams();
    QCOMPARE(flt.value(QStringLiteral("hpfEnabled")).toBool(), true);
    QCOMPARE(flt.value(QStringLiteral("lpfEnabled")).toBool(), false);
    QCOMPARE(flt.value(QStringLiteral("hpfFrequency")).toDouble(), 75.0);
    QCOMPARE(flt.value(QStringLiteral("hpfSlopeDbOct")).toInt(), 12);

    const QVariantMap comp = ctrl.getCompressorParams();
    QCOMPARE(comp.value(QStringLiteral("thresholdDb")).toDouble(), -18.0);
    QCOMPARE(comp.value(QStringLiteral("ratio")).toDouble(), 2.5);

    const QVariantMap lim = ctrl.getLimiterParams();
    QCOMPARE(lim.value(QStringLiteral("ceilingDb")).toDouble(), -1.0);
    QCOMPARE(lim.value(QStringLiteral("releaseMs")).toDouble(), 80.0);

    // Verify excluded items remained 100% untouched
    QCOMPARE(ctrl.getDspTrim(QStringLiteral("peq"), false), 2.5);
    QCOMPARE(ctrl.getDspTrim(QStringLiteral("compressor"), true), -3.0);
    QVERIFY(std::abs(ctrl.masterGain() - 0.75) < 1e-6);
    QCOMPARE(ctrl.isGeqGlobalBypass(), true);
    QCOMPARE(ctrl.listeningPresetName(), QStringLiteral("Harman Target"));

    // 2. Apply Per-DSP HPF "Vocal Cut" (18 dB/oct) — node ON/OFF & other DSPs must remain untouched
    QVERIFY(ctrl.applyPerDspPreset(QStringLiteral("hpf"), QStringLiteral("Vocal Cut")));
    const QVariantMap fltAfter = ctrl.getFilterParams();
    QCOMPARE(fltAfter.value(QStringLiteral("hpfFrequency")).toDouble(), 120.0);
    QCOMPARE(fltAfter.value(QStringLiteral("hpfSlopeDbOct")).toInt(), 18);
    QVERIFY(std::abs(fltAfter.value(QStringLiteral("hpfQ")).toDouble() - 0.80) < 1e-4);
    QCOMPARE(fltAfter.value(QStringLiteral("hpfEnabled")).toBool(), true);
    QCOMPARE(ctrl.getCompressorParams().value(QStringLiteral("thresholdDb")).toDouble(), -18.0);

    // 3. Apply Per-DSP Reverb "Hall" while Reverb node is OFF — stays OFF, params updated
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Reverb")));
    QVERIFY(ctrl.applyPerDspPreset(QStringLiteral("reverb"), QStringLiteral("Hall")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Reverb")));
    const QVariantMap revAfter = ctrl.getReverbParams();
    QCOMPARE(revAfter.value(QStringLiteral("decaySeconds")).toDouble(), 2.5);
    QCOMPARE(revAfter.value(QStringLiteral("predelayMs")).toDouble(), 35.0);
    QCOMPARE(revAfter.value(QStringLiteral("roomSize")).toDouble(), 0.85);

    // 4. Apply Global "Unity" — all 7 nodes OFF, all defaults restored, Trim/Master/LC/GEQ Global Bypass unchanged
    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Unity")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("PEQ")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("GEQ")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Compressor")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Limiter")));
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("Reverb")));
    QCOMPARE(ctrl.getFilterParams().value(QStringLiteral("hpfEnabled")).toBool(), false);
    QCOMPARE(ctrl.getFilterParams().value(QStringLiteral("lpfEnabled")).toBool(), false);
    QCOMPARE(ctrl.getDspTrim(QStringLiteral("peq"), false), 2.5);
    QCOMPARE(ctrl.isGeqGlobalBypass(), true);
    QCOMPARE(ctrl.listeningPresetName(), QStringLiteral("Harman Target"));
}

void TestDspPresets::testP3InvalidFilePreservesPriorState() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));
    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Big Band")));

    const QString brokenPath = m_tempDir->filePath(QStringLiteral("broken_preset.json"));
    {
        QFile f(brokenPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ \"format\": \"desk32-dsp-preset\", \"version\": 999, \"kind\": \"global\" }");
    }

    QVERIFY(!ctrl.applyDspPresetFromFile(brokenPath));
    QCOMPARE(ctrl.lastDspPresetReport().value(QStringLiteral("ok")).toBool(), false);
    QVERIFY(!ctrl.lastDspPresetReport().value(QStringLiteral("errorMessage")).toString().isEmpty());

    // Prior Big Band state completely preserved
    QCOMPARE(ctrl.activeGlobalPresetName(), QStringLiteral("Big Band"));
    QCOMPARE(ctrl.getCompressorParams().value(QStringLiteral("thresholdDb")).toDouble(), -15.0);
    QVERIFY(!ctrl.isDspNodeBypassed(QStringLiteral("GEQ")));
}

void TestDspPresets::testP3RenderPathReflectsGlobalPreset() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    // Apply Orchestra Hall (PEQ + Reverb + Limiter ON) and set Listening Correction to Harman Target
    ctrl.setListeningPreset(QStringLiteral("Harman Target"));
    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Orchestra Hall")));

    // Verify Render Monitor / Offline Render consistency & 0.0 Listening Correction leakage
    const QVariantMap res = ctrl.verifyRenderTapConsistency();
    QVERIFY2(res.value(QStringLiteral("consistent")).toBool(),
             qPrintable(QString("Render consistency failed: maxAbsDiff=%1 leakage=%2")
                            .arg(res.value(QStringLiteral("maxAbsDiff")).toDouble())
                            .arg(res.value(QStringLiteral("listeningLeakage")).toDouble())));
    QCOMPARE(res.value(QStringLiteral("listeningLeakage")).toDouble(), 0.0);
}

QTEST_MAIN(TestDspPresets)
#include "test_dsp_presets.moc"

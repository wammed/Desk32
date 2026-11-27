#include "BuiltInDspPresets.hpp"

#include <cmath>

namespace desk32::engine::preset {

namespace {

inline constexpr const char* kBuiltInTimestamp = "2026-11-27T00:00:00Z";

void setGeqFreq(GeqPresetData& geq, double freqHz, double gainDb) {
    for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
        if (std::abs(kGeqIsoFreqs[i] - freqHz) < 0.05) {
            geq.gainsDb[i] = gainDb;
            geq.bandEnabled[i] = true;
            return;
        }
    }
}

void setPeqBandLR(PeqPresetData& peq,
                  size_t bandIdx,
                  double freqHz,
                  double gainDb,
                  double q,
                  BiquadFilterType type) {
    if (bandIdx >= ParametricEQ::BANDS_PER_CHANNEL) return;
    PeqBandPreset b;
    b.freqHz = freqHz;
    b.gainDb = gainDb;
    b.q = q;
    b.type = type;
    b.enabled = true;
    peq.leftBands[bandIdx] = b;
    peq.rightBands[bandIdx] = b;
}

DspPresetDocument makeGlobalDoc(const QString& id, const QString& name, const GlobalDspPresetData& data) {
    DspPresetDocument doc;
    doc.format = QString::fromLatin1(kDspPresetFormat);
    doc.version = kDspPresetCurrentVersion;
    doc.kind = DspPresetKind::Global;
    doc.id = id;
    doc.name = name;
    doc.scope = DspPresetScope::Chain;
    doc.target = DspPresetTarget::Peq;
    doc.createdAt = QString::fromLatin1(kBuiltInTimestamp);
    doc.modifiedAt = QString::fromLatin1(kBuiltInTimestamp);
    doc.isFactory = true;
    doc.global = data;
    return doc;
}

DspPresetDocument makePerDspDoc(const QString& id,
                                const QString& name,
                                DspPresetTarget target,
                                const GlobalDspPresetData& holder) {
    DspPresetDocument doc;
    doc.format = QString::fromLatin1(kDspPresetFormat);
    doc.version = kDspPresetCurrentVersion;
    doc.kind = DspPresetKind::PerDsp;
    doc.id = id;
    doc.name = name;
    doc.scope = (target == DspPresetTarget::Peq) ? DspPresetScope::MasteringChain : DspPresetScope::Chain;
    doc.target = target;
    doc.createdAt = QString::fromLatin1(kBuiltInTimestamp);
    doc.modifiedAt = QString::fromLatin1(kBuiltInTimestamp);
    doc.isFactory = true;
    doc.global = holder;
    return doc;
}

std::vector<DspPresetDocument> buildAll48Presets() {
    std::vector<DspPresetDocument> out;
    out.reserve(48);

    // =========================================================================
    // A.12.1 Global Presets (6)
    // =========================================================================
    {
        // 1. Unity
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.unity"),
                                    QStringLiteral("Unity"),
                                    GlobalDspPresetData::unity()));
    }
    {
        // 2. Vocal Forward
        GlobalDspPresetData g = GlobalDspPresetData::unity();
        g.setNodeEnabled(DspPresetTarget::Peq, true);
        g.setNodeEnabled(DspPresetTarget::Hpf, true);
        g.setNodeEnabled(DspPresetTarget::Compressor, true);
        g.setNodeEnabled(DspPresetTarget::Limiter, true);
        g.peq.preampDb = 0.0;
        setPeqBandLR(g.peq, 0, 1200.0, 2.0, 1.2, BiquadFilterType::Peak);
        setPeqBandLR(g.peq, 1, 3200.0, 1.5, 1.5, BiquadFilterType::Peak);
        setPeqBandLR(g.peq, 2, 180.0, -1.5, 1.0, BiquadFilterType::Peak);
        g.hpf = {75.0, 0.71, 12, {}};
        g.compressor = {-18.0, 2.5, 12.0, 120.0, 4.0, 1.0, {}};
        g.limiter = {-1.0, 80.0, {}};
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.vocal_forward"),
                                    QStringLiteral("Vocal Forward"), g));
    }
    {
        // 3. Orchestra Hall
        GlobalDspPresetData g = GlobalDspPresetData::unity();
        g.setNodeEnabled(DspPresetTarget::Peq, true);
        g.setNodeEnabled(DspPresetTarget::Reverb, true);
        g.setNodeEnabled(DspPresetTarget::Limiter, true);
        g.peq.preampDb = 0.0;
        setPeqBandLR(g.peq, 0, 80.0, 1.5, 0.9, BiquadFilterType::LowShelf);
        setPeqBandLR(g.peq, 1, 6000.0, 1.0, 1.2, BiquadFilterType::HighShelf);
        g.reverb = {2.4, 35.0, 0.85, 0.45, 18.0, {}};
        g.limiter = {-1.5, 150.0, {}};
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.orchestra_hall"),
                                    QStringLiteral("Orchestra Hall"), g));
    }
    {
        // 4. Big Band
        GlobalDspPresetData g = GlobalDspPresetData::unity();
        g.setNodeEnabled(DspPresetTarget::Geq, true);
        g.setNodeEnabled(DspPresetTarget::Compressor, true);
        g.setNodeEnabled(DspPresetTarget::Limiter, true);
        setGeqFreq(g.geq, 100.0, 1.5);
        setGeqFreq(g.geq, 125.0, 1.5);
        setGeqFreq(g.geq, 2500.0, 2.0);
        setGeqFreq(g.geq, 3150.0, 2.0);
        setGeqFreq(g.geq, 4000.0, 2.0);
        g.compressor = {-15.0, 3.0, 8.0, 90.0, 3.0, 1.5, {}};
        g.limiter = {-1.0, 60.0, {}};
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.big_band"),
                                    QStringLiteral("Big Band"), g));
    }
    {
        // 5. Rock / Pop
        GlobalDspPresetData g = GlobalDspPresetData::unity();
        g.setNodeEnabled(DspPresetTarget::Geq, true);
        g.setNodeEnabled(DspPresetTarget::Compressor, true);
        g.setNodeEnabled(DspPresetTarget::Limiter, true);
        setGeqFreq(g.geq, 63.0, 3.0);
        setGeqFreq(g.geq, 80.0, 2.5);
        setGeqFreq(g.geq, 500.0, -1.5);
        setGeqFreq(g.geq, 8000.0, 2.5);
        setGeqFreq(g.geq, 10000.0, 3.0);
        setGeqFreq(g.geq, 12500.0, 3.0);
        g.compressor = {-14.0, 4.0, 5.0, 80.0, 3.0, 2.0, {}};
        g.limiter = {-0.5, 50.0, {}};
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.rock_pop"),
                                    QStringLiteral("Rock / Pop"), g));
    }
    {
        // 6. Chamber
        GlobalDspPresetData g = GlobalDspPresetData::unity();
        g.setNodeEnabled(DspPresetTarget::Peq, true);
        g.setNodeEnabled(DspPresetTarget::Reverb, true);
        g.peq.preampDb = 0.0;
        setPeqBandLR(g.peq, 0, 250.0, -1.0, 1.4, BiquadFilterType::Peak);
        setPeqBandLR(g.peq, 1, 4500.0, 1.0, 1.3, BiquadFilterType::Peak);
        g.reverb = {1.1, 18.0, 0.45, 0.55, 12.0, {}};
        out.push_back(makeGlobalDoc(QStringLiteral("builtin.global.chamber"),
                                    QStringLiteral("Chamber"), g));
    }

    // =========================================================================
    // A.12.2 GEQ Per-DSP Presets (6)
    // =========================================================================
    auto addGeq = [&](const QString& slug, const QString& name, const GeqPresetData& d) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.geq = d;
        out.push_back(makePerDspDoc(QStringLiteral("builtin.geq.") + slug, name, DspPresetTarget::Geq, h));
    };
    addGeq(QStringLiteral("unity"), QStringLiteral("Unity"), GeqPresetData::unity());
    {
        GeqPresetData d = GeqPresetData::unity();
        setGeqFreq(d, 20.0, 2.0);
        setGeqFreq(d, 25.0, 3.0);
        setGeqFreq(d, 31.5, 4.0);
        setGeqFreq(d, 40.0, 4.5);
        setGeqFreq(d, 50.0, 4.0);
        setGeqFreq(d, 63.0, 3.0);
        setGeqFreq(d, 80.0, 1.5);
        setGeqFreq(d, 100.0, 0.5);
        addGeq(QStringLiteral("bass_boost"), QStringLiteral("BASS BOOST"), d);
    }
    {
        GeqPresetData d = GeqPresetData::unity();
        setGeqFreq(d, 400.0, 1.0);
        setGeqFreq(d, 500.0, 1.5);
        setGeqFreq(d, 630.0, 2.0);
        setGeqFreq(d, 800.0, 2.5);
        setGeqFreq(d, 1000.0, 3.0);
        setGeqFreq(d, 1250.0, 2.5);
        setGeqFreq(d, 1600.0, 2.0);
        setGeqFreq(d, 2000.0, 1.0);
        addGeq(QStringLiteral("mid_boost"), QStringLiteral("MID BOOST"), d);
    }
    {
        GeqPresetData d = GeqPresetData::unity();
        setGeqFreq(d, 4000.0, 1.0);
        setGeqFreq(d, 5000.0, 1.5);
        setGeqFreq(d, 6300.0, 2.0);
        setGeqFreq(d, 8000.0, 3.0);
        setGeqFreq(d, 10000.0, 3.5);
        setGeqFreq(d, 12500.0, 4.0);
        setGeqFreq(d, 16000.0, 4.5);
        setGeqFreq(d, 20000.0, 4.0);
        addGeq(QStringLiteral("treble_boost"), QStringLiteral("TREBLE BOOST"), d);
    }
    {
        GeqPresetData d = GeqPresetData::unity();
        setGeqFreq(d, 31.5, 3.5);
        setGeqFreq(d, 40.0, 4.5);
        setGeqFreq(d, 50.0, 4.5);
        setGeqFreq(d, 63.0, 4.0);
        setGeqFreq(d, 80.0, 2.5);
        setGeqFreq(d, 500.0, -2.0);
        setGeqFreq(d, 630.0, -2.5);
        setGeqFreq(d, 800.0, -2.0);
        setGeqFreq(d, 1000.0, -1.5);
        setGeqFreq(d, 8000.0, 3.0);
        setGeqFreq(d, 10000.0, 4.0);
        setGeqFreq(d, 12500.0, 4.5);
        setGeqFreq(d, 16000.0, 4.0);
        addGeq(QStringLiteral("bass_treble_boost"), QStringLiteral("BASS & TREBLE BOOST"), d);
    }
    {
        GeqPresetData d = GeqPresetData::unity();
        setGeqFreq(d, 250.0, 1.0);
        setGeqFreq(d, 315.0, 1.0);
        setGeqFreq(d, 4000.0, -1.5);
        setGeqFreq(d, 5000.0, -2.0);
        setGeqFreq(d, 6300.0, -2.5);
        setGeqFreq(d, 8000.0, -3.0);
        setGeqFreq(d, 10000.0, -3.5);
        setGeqFreq(d, 12500.0, -4.0);
        setGeqFreq(d, 16000.0, -4.5);
        setGeqFreq(d, 20000.0, -5.0);
        addGeq(QStringLiteral("mellow"), QStringLiteral("MELLOW"), d);
    }

    // =========================================================================
    // A.12.3 PEQ Per-DSP Presets (6 / Mastering Chain only)
    // =========================================================================
    auto addPeq = [&](const QString& slug, const QString& name, const PeqPresetData& d) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.peq = d;
        out.push_back(makePerDspDoc(QStringLiteral("builtin.peq.") + slug, name, DspPresetTarget::Peq, h));
    };
    addPeq(QStringLiteral("unity"), QStringLiteral("Unity"), PeqPresetData::unity());
    {
        PeqPresetData d = PeqPresetData::unity();
        d.preampDb = -5.5;
        setPeqBandLR(d, 0, 35.0, 4.5, 0.8, BiquadFilterType::LowShelf);
        setPeqBandLR(d, 1, 65.0, 3.0, 1.4, BiquadFilterType::Peak);
        addPeq(QStringLiteral("bass_boost"), QStringLiteral("BASS BOOST"), d);
    }
    {
        PeqPresetData d = PeqPresetData::unity();
        d.preampDb = -2.5;
        setPeqBandLR(d, 0, 750.0, 2.5, 1.1, BiquadFilterType::Peak);
        setPeqBandLR(d, 1, 1800.0, 2.0, 1.4, BiquadFilterType::Peak);
        addPeq(QStringLiteral("mid_boost"), QStringLiteral("MID BOOST"), d);
    }
    {
        PeqPresetData d = PeqPresetData::unity();
        d.preampDb = -5.0;
        setPeqBandLR(d, 0, 8000.0, 4.0, 0.8, BiquadFilterType::HighShelf);
        setPeqBandLR(d, 1, 14000.0, 2.5, 1.6, BiquadFilterType::Peak);
        addPeq(QStringLiteral("treble_boost"), QStringLiteral("TREBLE BOOST"), d);
    }
    {
        PeqPresetData d = PeqPresetData::unity();
        d.preampDb = -4.0;
        setPeqBandLR(d, 0, 45.0, 4.0, 0.8, BiquadFilterType::LowShelf);
        setPeqBandLR(d, 1, 800.0, -2.5, 1.2, BiquadFilterType::Peak);
        setPeqBandLR(d, 2, 10000.0, 4.0, 0.9, BiquadFilterType::HighShelf);
        addPeq(QStringLiteral("bass_treble_boost"), QStringLiteral("BASS & TREBLE BOOST"), d);
    }
    {
        PeqPresetData d = PeqPresetData::unity();
        d.preampDb = 0.0;
        setPeqBandLR(d, 0, 220.0, 1.5, 1.0, BiquadFilterType::Peak);
        setPeqBandLR(d, 1, 6500.0, -3.5, 0.8, BiquadFilterType::HighShelf);
        addPeq(QStringLiteral("mellow"), QStringLiteral("MELLOW"), d);
    }

    // =========================================================================
    // A.12.4 HPF Per-DSP Presets (6)
    // =========================================================================
    auto addHpf = [&](const QString& slug, const QString& name, double cutoff, double q, int slope) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.hpf = {cutoff, q, slope, {}};
        out.push_back(makePerDspDoc(QStringLiteral("builtin.hpf.") + slug, name, DspPresetTarget::Hpf, h));
    };
    addHpf(QStringLiteral("unity"), QStringLiteral("Unity"), 20.0, 0.71, 12);
    addHpf(QStringLiteral("subsonic_guard"), QStringLiteral("Subsonic Guard"), 20.0, 0.71, 24);
    addHpf(QStringLiteral("rumble_cut"), QStringLiteral("Rumble Cut"), 35.0, 0.71, 24);
    addHpf(QStringLiteral("clean_low"), QStringLiteral("Clean Low"), 60.0, 0.71, 12);
    addHpf(QStringLiteral("vocal_cut"), QStringLiteral("Vocal Cut"), 120.0, 0.80, 18);
    addHpf(QStringLiteral("tight_bass"), QStringLiteral("Tight Bass"), 45.0, 1.00, 24);

    // =========================================================================
    // A.12.5 LPF Per-DSP Presets (6)
    // =========================================================================
    auto addLpf = [&](const QString& slug, const QString& name, double cutoff, double q, int slope) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.lpf = {cutoff, q, slope, {}};
        out.push_back(makePerDspDoc(QStringLiteral("builtin.lpf.") + slug, name, DspPresetTarget::Lpf, h));
    };
    addLpf(QStringLiteral("unity"), QStringLiteral("Unity"), 20000.0, 0.71, 12);
    addLpf(QStringLiteral("air_trim"), QStringLiteral("Air Trim"), 18000.0, 0.71, 12);
    addLpf(QStringLiteral("anti_harsh"), QStringLiteral("Anti-Harsh"), 12000.0, 0.71, 12);
    addLpf(QStringLiteral("tape_soft"), QStringLiteral("Tape Soft"), 14000.0, 0.85, 18);
    addLpf(QStringLiteral("vinyl_warm"), QStringLiteral("Vinyl Warm"), 10000.0, 0.75, 12);
    addLpf(QStringLiteral("night"), QStringLiteral("Night"), 7500.0, 0.71, 12);

    // =========================================================================
    // A.12.6 Compressor Per-DSP Presets (6)
    // =========================================================================
    auto addComp = [&](const QString& slug, const QString& name,
                       double thresh, double ratio, double att, double rel, double knee, double makeup) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.compressor = {thresh, ratio, att, rel, knee, makeup, {}};
        out.push_back(makePerDspDoc(QStringLiteral("builtin.compressor.") + slug, name, DspPresetTarget::Compressor, h));
    };
    addComp(QStringLiteral("unity"), QStringLiteral("Unity"), 0.0, 1.0, 10.0, 100.0, 0.0, 0.0);
    addComp(QStringLiteral("gentle_glue"), QStringLiteral("Gentle Glue"), -16.0, 2.0, 15.0, 150.0, 6.0, 1.0);
    addComp(QStringLiteral("vocal_ride"), QStringLiteral("Vocal Ride"), -20.0, 3.0, 10.0, 120.0, 4.0, 1.5);
    addComp(QStringLiteral("punch"), QStringLiteral("Punch"), -14.0, 4.0, 5.0, 70.0, 2.0, 2.0);
    addComp(QStringLiteral("squash"), QStringLiteral("Squash"), -24.0, 8.0, 2.0, 60.0, 1.0, 3.0);
    addComp(QStringLiteral("transparent"), QStringLiteral("Transparent"), -18.0, 1.5, 25.0, 250.0, 6.0, 0.5);

    // =========================================================================
    // A.12.7 Limiter Per-DSP Presets (6 / Sample-Peak)
    // =========================================================================
    auto addLim = [&](const QString& slug, const QString& name, double ceilingDbfs, double relMs) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.limiter = {ceilingDbfs, relMs, {}};
        out.push_back(makePerDspDoc(QStringLiteral("builtin.limiter.") + slug, name, DspPresetTarget::Limiter, h));
    };
    addLim(QStringLiteral("unity"), QStringLiteral("Unity"), 0.0, 50.0);
    addLim(QStringLiteral("safe_ceiling"), QStringLiteral("Safe Ceiling"), -1.0, 80.0);
    addLim(QStringLiteral("standard"), QStringLiteral("Standard"), -0.3, 50.0);
    addLim(QStringLiteral("broadcast"), QStringLiteral("Broadcast"), -1.5, 100.0);
    addLim(QStringLiteral("hot"), QStringLiteral("Hot"), -0.5, 40.0);
    addLim(QStringLiteral("clip_guard"), QStringLiteral("Clip Guard"), -0.1, 30.0);

    // =========================================================================
    // A.12.8 Reverb Per-DSP Presets (6)
    // =========================================================================
    auto addRev = [&](const QString& slug, const QString& name,
                      double decayS, double preMs, double size, double damp, double wetPct) {
        GlobalDspPresetData h = GlobalDspPresetData::unity();
        h.reverb = {decayS, preMs, size, damp, wetPct, {}};
        out.push_back(makePerDspDoc(QStringLiteral("builtin.reverb.") + slug, name, DspPresetTarget::Reverb, h));
    };
    addRev(QStringLiteral("unity"), QStringLiteral("Unity"), 0.2, 0.0, 0.10, 0.50, 0.0);
    addRev(QStringLiteral("small_room"), QStringLiteral("Small Room"), 0.6, 8.0, 0.25, 0.60, 12.0);
    addRev(QStringLiteral("studio"), QStringLiteral("Studio"), 1.1, 15.0, 0.45, 0.50, 15.0);
    addRev(QStringLiteral("hall"), QStringLiteral("Hall"), 2.5, 35.0, 0.85, 0.40, 20.0);
    addRev(QStringLiteral("cathedral"), QStringLiteral("Cathedral"), 4.2, 55.0, 0.95, 0.35, 25.0);
    addRev(QStringLiteral("plate"), QStringLiteral("Plate"), 1.8, 20.0, 0.65, 0.30, 16.0);

    return out;
}

} // namespace

const std::vector<DspPresetDocument>& BuiltInDspPresets::allPresets() {
    static const std::vector<DspPresetDocument> s_all = buildAll48Presets();
    return s_all;
}

std::vector<DspPresetDocument> BuiltInDspPresets::presetsFor(DspPresetKind kind,
                                                             DspPresetTarget target) {
    std::vector<DspPresetDocument> res;
    for (const auto& doc : allPresets()) {
        if (doc.kind != kind) continue;
        if (kind == DspPresetKind::PerDsp && doc.target != target) continue;
        res.push_back(doc);
    }
    return res;
}

bool BuiltInDspPresets::findPreset(DspPresetKind kind,
                                   DspPresetTarget target,
                                   const QString& idOrName,
                                   DspPresetDocument& outDoc) {
    for (const auto& doc : allPresets()) {
        if (doc.kind != kind) continue;
        if (kind == DspPresetKind::PerDsp && doc.target != target) continue;
        if (doc.id == idOrName || doc.name == idOrName) {
            outDoc = doc;
            return true;
        }
    }
    return false;
}

bool BuiltInDspPresets::isBuiltIn(DspPresetKind kind,
                                  DspPresetTarget target,
                                  const QString& idOrName) {
    DspPresetDocument dummy;
    return findPreset(kind, target, idOrName, dummy);
}

DspPresetDocument BuiltInDspPresets::globalUnity() {
    DspPresetDocument doc;
    (void)findPreset(DspPresetKind::Global, DspPresetTarget::Peq, QStringLiteral("builtin.global.unity"), doc);
    return doc;
}

DspPresetDocument BuiltInDspPresets::perDspUnity(DspPresetTarget target) {
    DspPresetDocument doc;
    (void)findPreset(DspPresetKind::PerDsp, target, QStringLiteral("Unity"), doc);
    return doc;
}

} // namespace desk32::engine::preset

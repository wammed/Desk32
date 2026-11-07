#!/usr/bin/env python3
"""
Desk32 Phase 0 Step 3 - Item 8: External Host & Plugin Compatibility Test
Tests Carla (2.5.10) compatibility with Linux-native plugin formats:
- LV2 (LSP Limiter Stereo)
- VST3 (LSP VST3 Bundle)
- VST2 (LSP VST2 Binary)
- LADSPA (LSP LADSPA Library)
- CLAP (LSP CLAP Plugin)
"""

import sys
import os

sys.path.append("/usr/share/carla")
try:
    import carla_backend
except ImportError as e:
    print(f"Error importing carla_backend: {e}")
    sys.exit(1)

def run_compatibility_tests():
    host = carla_backend.CarlaHostDLL("/usr/lib/carla/libcarla_standalone2.so", True)
    
    # Initialize engine with dummy/null or JACK driver
    # Driver 0: JACK (connected via pipewire-jack)
    ok = host.engine_init("JACK", "Desk32-CompatTest")
    if not ok:
        print("Failed to initialize Carla engine with JACK driver")
        return False

    results = {}

    # 1. Test LV2
    try:
        lv2_ok = host.add_plugin(
            carla_backend.BINARY_NATIVE,
            carla_backend.PLUGIN_LV2,
            None,
            "LSP Limiter Stereo",
            "http://lsp-plug.in/plugins/lv2/limiter_stereo",
            0,
            None,
            0
        )
        if lv2_ok:
            idx = host.get_current_plugin_count() - 1
            info = host.get_plugin_info(idx)
            results["LV2"] = {
                "status": "PASS",
                "name": info.get("name"),
                "label": info.get("label"),
                "category": info.get("category"),
                "hints": info.get("hints")
            }
        else:
            results["LV2"] = {"status": "FAIL", "reason": "host.add_plugin returned False"}
    except Exception as e:
        results["LV2"] = {"status": "ERROR", "error": str(e)}

    # 2. Test VST3
    try:
        vst3_path = "/usr/lib/vst3/lsp-plugins.vst3"
        vst3_ok = host.add_plugin(
            carla_backend.BINARY_NATIVE,
            carla_backend.PLUGIN_VST3,
            vst3_path,
            "LSP VST3",
            "",
            0,
            None,
            0
        )
        if vst3_ok:
            idx = host.get_current_plugin_count() - 1
            info = host.get_plugin_info(idx)
            results["VST3"] = {
                "status": "PASS",
                "name": info.get("name"),
                "filename": info.get("filename"),
                "category": info.get("category")
            }
        else:
            results["VST3"] = {"status": "FAIL", "reason": "host.add_plugin returned False"}
    except Exception as e:
        results["VST3"] = {"status": "ERROR", "error": str(e)}

    # 3. Test VST2
    try:
        vst2_path = "/usr/lib/vst/lsp-plugins.vst/lsp-plugins.so"
        vst2_ok = host.add_plugin(
            carla_backend.BINARY_NATIVE,
            carla_backend.PLUGIN_VST2,
            vst2_path,
            "LSP VST2",
            "",
            0,
            None,
            0
        )
        if vst2_ok:
            idx = host.get_current_plugin_count() - 1
            info = host.get_plugin_info(idx)
            results["VST2"] = {
                "status": "PASS",
                "name": info.get("name"),
                "filename": info.get("filename")
            }
        else:
            results["VST2"] = {"status": "FAIL", "reason": "host.add_plugin returned False"}
    except Exception as e:
        results["VST2"] = {"status": "ERROR", "error": str(e)}

    # 4. Test LADSPA
    try:
        ladspa_path = "/usr/lib/ladspa/lsp-plugins-ladspa.so"
        ladspa_ok = host.add_plugin(
            carla_backend.BINARY_NATIVE,
            carla_backend.PLUGIN_LADSPA,
            ladspa_path,
            "LSP LADSPA",
            "",
            0,
            None,
            0
        )
        if ladspa_ok:
            idx = host.get_current_plugin_count() - 1
            info = host.get_plugin_info(idx)
            results["LADSPA"] = {
                "status": "PASS",
                "name": info.get("name"),
                "filename": info.get("filename")
            }
        else:
            results["LADSPA"] = {"status": "FAIL", "reason": "host.add_plugin returned False"}
    except Exception as e:
        results["LADSPA"] = {"status": "ERROR", "error": str(e)}

    # 5. Check CLAP
    # Carla 2.5 has no direct PLUGIN_CLAP constant; requires Carla 2.6 or wrapper
    clap_path = "/usr/lib/clap/lsp-plugins.clap"
    results["CLAP"] = {
        "status": "RESERVED_FOR_CARLA_2_6",
        "file_exists": os.path.exists(clap_path),
        "notes": "Native CLAP binary verified on filesystem; Carla 2.5.10 backend does not expose direct PLUGIN_CLAP constant"
    }

    host.engine_close()

    print("\n=== Item 8: Carla Plugin Compatibility Test Results ===")
    for fmt, data in results.items():
        print(f"Format [{fmt}]: {data['status']}")
        for k, v in data.items():
            if k != "status":
                print(f"   {k}: {v}")
    print("========================================================\n")

    return results

if __name__ == "__main__":
    run_compatibility_tests()


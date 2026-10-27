#!/usr/bin/env python3
"""
SOFA to Binary Converter for DM8168 HRTF Spatial Audio Processing
==================================================================
Offline tool: parse MIT KEMAR normal_pinna SOFA (HDF5/netCDF) file,
extract HRIR data and azimuth mapping into binary + C header.

Outputs:
  - hrir_table.bin : raw float32 HRIR data [num_azimuths][HRIR_LENGTH][2(L/R)]
  - azimuth_map.h  : C header with azimuth index table and constants

Usage:
  python3 sofa_to_bin.py [input.sofa] [output_dir]

Requires: h5py, numpy
  pip install h5py numpy
"""

import sys
import os
import struct
import numpy as np

try:
    import h5py
except ImportError:
    print("Error: h5py not installed. Run: pip install h5py numpy")
    sys.exit(1)

# --- Constants (must match shared/AppCommon.h) ---
HRIR_LENGTH = 256
MAX_AZIMUTHS = 710


def parse_sofa(sofa_path, output_dir):
    """Parse SOFA file and extract HRIR data + azimuth mapping."""
    print(f"[INFO] Opening SOFA file: {sofa_path}")

    with h5py.File(sofa_path, 'r') as f:
        # --- Print SOFA metadata ---
        title = _get_attr(f, 'Title', 'Unknown')
        db_name = _get_attr(f, 'DatabaseName', 'Unknown')
        listener_short = _get_attr(f, 'ListenerShortName', 'Unknown')
        conv = _get_attr(f, 'SOFAConventions', 'Unknown')
        conv_ver = _get_attr(f, 'SOFAConventionsVersion', 'Unknown')
        data_type = _get_attr(f, 'DataType', 'FIR')
        room_type = _get_attr(f, 'RoomType', 'Unknown')

        print(f"  Title:             {title}")
        print(f"  DatabaseName:      {db_name}")
        print(f"  Listener:          {listener_short}")
        print(f"  Convention:        {conv} v{conv_ver}")
        print(f"  DataType:          {data_type}")
        print(f"  RoomType:          {room_type}")

        # --- Extract Data.IR (the HRIR data) ---
        if 'Data.IR' not in f:
            print("[ERROR] Data.IR not found in SOFA file!")
            sys.exit(1)

        ir_data = f['Data.IR'][:]  # shape: (num_measurements, num_ears, num_samples)
        print(f"  Data.IR shape:     {ir_data.shape}")
        num_meas, num_ears, num_samples = ir_data.shape
        assert num_ears == 2, f"Expected 2 ears (L/R), got {num_ears}"

        # --- Extract Data.SamplingRate ---
        if 'Data.SamplingRate' in f:
            sr_data = f['Data.SamplingRate'][:]
            sample_rate = int(sr_data.flat[0])
            print(f"  SamplingRate:      {sample_rate} Hz")
        else:
            sample_rate = 48000
            print(f"  SamplingRate:      (not found, default {sample_rate})")

        # --- Extract SourcePosition (azimuth, elevation, distance) ---
        if 'SourcePosition' not in f:
            print("[ERROR] SourcePosition not found in SOFA file!")
            sys.exit(1)

        src_pos = f['SourcePosition'][:]  # shape: (num_measurements, 3) = [az, el, dist]
        print(f"  SourcePosition:    {src_pos.shape}")
        assert src_pos.shape[0] == num_meas, "SourcePosition count mismatch"

        azimuths = src_pos[:, 0]   # azimuth in degrees
        elevations = src_pos[:, 1]  # elevation in degrees
        distances = src_pos[:, 2]  # distance in meters

        # --- Extract Data.Delay (optional) ---
        delays = None
        if 'Data.Delay' in f:
            delays = f['Data.Delay'][:]
            print(f"  Data.Delay shape:  {delays.shape}")

    # --- Build azimuth index table ---
    # For horizontal plane (elevation ~ 0 deg), find all measurements
    # If no exact 0-deg elevation, use the closest ring
    print("\n[INFO] Building azimuth index table...")

    # Find unique elevations
    unique_elev = np.unique(np.round(elevations, 1))
    print(f"  Unique elevations ({len(unique_elev)}): {unique_elev[:10]}{'...' if len(unique_elev) > 10 else ''}")

    # Select horizontal ring (elevation closest to 0)
    hor_elev_idx = np.argmin(np.abs(unique_elev))
    hor_elev = unique_elev[hor_elev_idx]
    print(f"  Using elevation ring: {hor_elev} deg")

    # Get indices of measurements at this elevation
    hor_mask = np.abs(elevations - hor_elev) < 1.0
    hor_indices = np.where(hor_mask)[0]
    hor_azimuths = azimuths[hor_indices]

    print(f"  Horizontal ring measurements: {len(hor_indices)}")
    print(f"  Azimuth range: {hor_azimuths.min():.1f} to {hor_azimuths.max():.1f} deg")

    # Sort by azimuth angle
    sort_order = np.argsort(hor_azimuths)
    hor_indices = hor_indices[sort_order]
    hor_azimuths = hor_azimuths[sort_order]

    num_hor_az = len(hor_indices)
    print(f"  Sorted azimuths: {hor_azimuths[:8]}{'...' if num_hor_az > 8 else ''}")

    # --- Truncate/pad HRIR to HRIR_LENGTH ---
    print(f"\n[INFO] Processing HRIR data (truncate/pad to {HRIR_LENGTH} samples)...")
    hrir_table = np.zeros((num_hor_az, 2, HRIR_LENGTH), dtype=np.float32)

    for i, meas_idx in enumerate(hor_indices):
        for ear in range(2):
            hrir_raw = ir_data[meas_idx, ear, :]
            if len(hrir_raw) >= HRIR_LENGTH:
                hrir_table[i, ear, :] = hrir_raw[:HRIR_LENGTH]
            else:
                hrir_table[i, ear, :len(hrir_raw)] = hrir_raw

    print(f"  HRIR table shape: {hrir_table.shape}")
    print(f"  HRIR value range: [{hrir_table.min():.6f}, {hrir_table.max():.6f}]")

    # --- Write binary file ---
    bin_path = os.path.join(output_dir, 'hrir_table.bin')
    print(f"\n[INFO] Writing HRIR binary: {bin_path}")
    with open(bin_path, 'wb') as bf:
        # Header: magic(4) + version(4) + num_azimuths(4) + hrir_len(4) + num_ears(4) + sample_rate(4)
        header = struct.pack('<4sIIIII',
                            b'HRIR',
                            1,                    # version
                            num_hor_az,           # number of azimuths
                            HRIR_LENGTH,          # samples per HRIR
                            2,                    # ears (L/R)
                            sample_rate)          # sampling rate
        bf.write(header)
        # Data: num_hor_az × 2 × HRIR_LENGTH float32 values
        # Layout: [az0_L[0..255], az0_R[0..255], az1_L[0..255], az1_R[0..255], ...]
        for i in range(num_hor_az):
            for ear in range(2):
                bf.write(hrir_table[i, ear, :].tobytes())

    bin_size = os.path.getsize(bin_path)
    print(f"  Binary file size: {bin_size} bytes ({bin_size/1024:.1f} KB)")

    # --- Write azimuth_map.h ---
    h_path = os.path.join(output_dir, 'azimuth_map.h')
    print(f"[INFO] Writing C header: {h_path}")
    with open(h_path, 'w') as hf:
        hf.write("/* Auto-generated by sofa_to_bin.py - DO NOT EDIT */\n")
        hf.write(f"/* Source: {os.path.basename(sofa_path)} */\n")
        hf.write(f"/* Horizontal ring elevation: {hor_elev} deg */\n\n")
        hf.write("#ifndef AZIMUTH_MAP_H_\n")
        hf.write("#define AZIMUTH_MAP_H_\n\n")
        hf.write(f"#define NUM_AZIMUTHS          {num_hor_az}\n")
        hf.write(f"#define HRIR_LENGTH           {HRIR_LENGTH}\n")
        hf.write(f"#define HRIR_SAMPLE_RATE      {sample_rate}\n\n")
        hf.write("/* Azimuth angles in degrees, sorted ascending */\n")
        hf.write(f"static const float azimuth_degrees[{num_hor_az}] = {{\n")
        for i in range(num_hor_az):
            if i % 8 == 0:
                hf.write("    ")
            hf.write(f"{hor_azimuths[i]:.1f}")
            if i < num_hor_az - 1:
                hf.write(", ")
            if (i + 1) % 8 == 0 or i == num_hor_az - 1:
                hf.write("\n")
        hf.write("};\n\n")
        hf.write("#endif /* AZIMUTH_MAP_H_ */\n")

    print(f"\n[DONE] Successfully generated:")
    print(f"  {bin_path} ({bin_size} bytes)")
    print(f"  {h_path}")
    print(f"  Num azimuths: {num_hor_az}")
    print(f"  HRIR length: {HRIR_LENGTH}")
    print(f"  Sample rate: {sample_rate}")

    return num_hor_az


def _get_attr(h5obj, name, default=''):
    """Get string attribute from HDF5 object."""
    if name in h5obj.attrs:
        val = h5obj.attrs[name]
        if isinstance(val, bytes):
            return val.decode('utf-8').strip()
        return str(val).strip()
    return default


if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))

    if len(sys.argv) >= 3:
        sofa_path = sys.argv[1]
        output_dir = sys.argv[2]
    elif len(sys.argv) >= 2:
        sofa_path = sys.argv[1]
        output_dir = script_dir
    else:
        # Default: look for .sofa in parent directory
        default_sofa = os.path.join(script_dir, '..', 'mit_kemar_normal_pinna.sofa')
        if os.path.exists(default_sofa):
            sofa_path = default_sofa
            output_dir = script_dir
        else:
            print("Usage: python3 sofa_to_bin.py [input.sofa] [output_dir]")
            print("  Place mit_kemar_normal_pinna.sofa in the project root.")
            sys.exit(1)

    sofa_path = os.path.abspath(sofa_path)
    output_dir = os.path.abspath(output_dir)
    os.makedirs(output_dir, exist_ok=True)

    if not os.path.exists(sofa_path):
        print(f"[ERROR] SOFA file not found: {sofa_path}")
        sys.exit(1)

    parse_sofa(sofa_path, output_dir)

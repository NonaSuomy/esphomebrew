# Duke3D build profiles

Both builds use the same app adapter in this directory. The manifest retains the original shareware engine pin and its sound-effects-only music profile. The native builder defines `PAX_DUKE_MUSIC=1` and links the newer engine MIDI backend, OPL helpers, MP3 decoder and FluidSynth. Both use the current 48 kHz output resampler. This feature flag avoids requiring the newer music engine symbols in the older pinned manifest engine; it does not select another adapter tree.

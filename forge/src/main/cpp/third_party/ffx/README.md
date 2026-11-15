# AMD FidelityFX Super Resolution 1 headers

`ffx_a.h` and `ffx_fsr1.h` are vendored from AMD's FidelityFX FSR 1 release and
are distributed under the MIT license; the license text is included at the top
of each header. They are used only by the compute shader sources under
`app/src/main/cpp/shaders/` to generate the checked-in SPIR-V modules.

No AMD runtime library or binary is included.

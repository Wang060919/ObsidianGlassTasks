# Third-party notices

- **markdown-it 15.0.3** — MIT. Vendored browser runtime retained from the existing widget. See `resources/licenses/markdown-it-MIT.txt`.
- **Microsoft WebView2 SDK 1.0.4258.31** — Microsoft SDK license and notices are retained in `resources/licenses/WebView2-SDK-LICENSE.txt` and `WebView2-SDK-NOTICE.txt`. The x64 static loader is linked; the separately installed Edge WebView2 Runtime renders local content.
- **shayann07/liquidglass** — Apache-2.0, Copyright 2026 Shayan. The initial `src/Glass.hlsl` port adapted the measured resting-corner map from `ports/skia/shaders/material.sksl`. Version 0.1.5 replaces that table and its corner weighting with one continuous Snell refraction profile for straight edges and corners. The project remains a reference for material tint and diffusion; the Windows desktop input, Gaussian filter, simplified lighting and pressure response are local implementations, not validated Apple parity. Upstream attribution and the full LICENSE and NOTICE are retained in `resources/licenses/liquidglass-*`.

Windows desktop duplication and composition are implemented against public Microsoft APIs. `poncippg-spec/liquidDX11` was consulted as a capture architecture reference; its ImGui application and shader implementation are not redistributed. No Apple code, symbols, system images or reference captures are shipped.

Project references:

- https://github.com/shayann07/liquidglass
- https://github.com/poncippg-spec/liquidDX11
- https://github.com/markdown-it/markdown-it
- https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4258.31

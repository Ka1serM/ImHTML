# Third-party notices

ImHTML itself is MIT licensed (see [LICENSE](LICENSE)). It does not vendor any
of the libraries below — the embedding project supplies them — but a binary
built from ImHTML links them, so their terms apply to that binary.

| Library | Used by | Licence |
| --- | --- | --- |
| [Dear ImGui](https://github.com/ocornut/imgui) | `ImHTML::Core` — draw lists, fonts, input state | MIT |
| [litehtml](https://github.com/litehtml/litehtml) | `ImHTML::Core` — HTML parsing, CSS cascade, layout, hit testing | BSD-3-Clause |
| [gumbo-parser](https://github.com/google/gumbo-parser) | pulled in by litehtml | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | `ImHTML::Core` — data for list templates | MIT |
| [inja](https://github.com/pantor/inja) | `ImHTML::Core` — template expansion (implementation only) | MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | `ImHTML::App` — window scale, clipboard, IME, URL opening | Zlib |
| [lunasvg](https://github.com/sammycage/lunasvg) | `ImHTML::App` — SVG rasterization for the built-in image cache | MIT |

`ImHTML::Core` links ImGui, litehtml and nlohmann/json. SDL3 and lunasvg are
required only by the optional `ImHTML::App` shell; a host that drives the core
directly needs neither.

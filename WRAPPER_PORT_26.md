# Bionic wrapper → Mesa 26.2.4

Распаковать поверх **чистой** Mesa 26.2.4 (`mesa-mesa-26.2.4`). Сборкой не проверялось.
Слой Mesa/WSI — подход wrapper-25 (апстримный код + точечные `__TERMUX__`-патчи), слой враппера — наш исправленный bionic.

## Обвязка (легла чисто, с мелкими правками под 26.x)
| Файл | Что |
|---|---|
| `meson.options` | `'wrapper'` в `vulkan-drivers` (в 26.x опции в `meson.options`, не `meson_options.txt`) |
| `meson.build` | `with_wrapper_vk`; для враппера всегда `with_android_stub` (в 26.x появилась проверка `need_android_stub` — обойдена через `elif`) |
| `src/vulkan/meson.build` | `VK_USE_PLATFORM_ANDROID_KHR` и `subdir('wrapper')` |
| `src/vulkan/wsi/meson.build` | `wsi_common_android.c` |
| `src/util/rand_xor.c` | без `getrandom` под `__TERMUX__` |
| `src/util/anon_file.c` | **не нужен**: в 26.x свой fallback, на Android-сборке memfd через NDK; sed в CI убран |
| `src/android_stub/nativewindow_stub.cpp` | заглушка `AHardwareBuffer_sendHandleToUnixSocket` |
| `src/vulkan/runtime/vk_object.h` | assert типа объекта выключен (как в bionic, только debug) |

## WSI (адаптировано)
- `wsi_common.c/.h/_private.h`: тип образа `WSI_IMAGE_TYPE_ANDROID`, AHB в `wsi_image`/`wsi_image_info`, `forcesync`/`blit` (`MESA_VK_WSI_DEBUG`), `WRAPPER_BLIT`, `force_rgba8_unorm_first`, сигнал семафоров/фенсов через импорт sync-fd `-1` (семафоры принадлежат драйверу), `CmdBlitImage` для blit в AHB другого формата.
- `wsi_common_android.c` — новый: структура из v25, проверка импортируемости AHB из bionic, исправлены ошибки v25 (не проверялся `AllocateMemory`, RGBA8 отсутствовал в маппинге bionic).
- `wsi_common_x11.c` (всё под `__TERMUX__`):
  - AHB → pixmap через unix socket (`AHardwareBuffer_sendHandleToUnixSocket`) с ожиданием ack;
  - XSync-фенсы вместо xshmfence;
  - XFIXES **опционален** (в v25 был вырезан целиком; здесь регионы damage используются, только если сервер их поддерживает);
  - RGBA8-форматы + порядок по `WRAPPER_SURFACE_FORMAT`, `WRAPPER_MAX_IMAGE_COUNT`;
  - `_MESA_DRV*` свойства окна для HUD эмулятора;
  - проверка смены DRM-модификаторов отключена (AHB их не использует).

## Враппер
- `src/vulkan/wrapper/` — наш bionic-враппер со всеми исправлениями.
- `vk_wrapper_features_gen.py` — заново форкнут от `vk_physical_device_features_gen.py` 26.2.4 (старый падал на vk.xml 1.4.354). Остальные генераторы (entrypoints, trampolines, unwrappers, printers) на 1.4.354 отрабатывают без изменений — проверено.
- `meson.build`: новый CLI `vk_icd_gen.py` (`--icd-lib-path` + `--icd-filename`), ICD api 1.4. Путь в JSON прежний: `<prefix>/<libdir>/libvulkan_wrapper.so`.
- Новое под WSI 26: `KHR_swapchain_maintenance1`, `KHR_present_id2`, `KHR_present_wait2` (wait2 только при наличии present_wait/timeline), `EXT_present_timing` отфильтрован, их feature-структуры не уходят в драйвер.
- apiVersion теперь ограничивается заголовком 1.4.354 (`WRAPPER_VK_VERSION` по-прежнему работает).
- `vk_physical_device::properties` заполняется (apiVersion, timestampPeriod и т.д.) — рантайм 26 их читает.
- `vkQueueSubmit2` fallback на `vkQueueSubmit`: WSI 26 отправляет present/blit только через Submit2, старые драйверы Adreno без sync2 упали бы.

## CI
`.github/workflows/build.yml`, `android.toml`, `shims.zip` перенесены; sed для `anon_file.c` убран.
Может понадобиться в shims: заголовки/`.pc` для новых зависимостей X11-части 26.x (`loader_x11`, xcb-randr/xfixes/keysyms) — если meson/компилятор чего-то не найдёт, это первое место, куда смотреть.

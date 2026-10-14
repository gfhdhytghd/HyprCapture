# Pinned Hyprland source evidence

Inspected git objects at `efb50993780079460b0cbed1363e2166a2de1d9f` (live 0.56.2), not the current checkout HEAD.

## src/protocols/core/Shm.cpp:220

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/protocols/core/Shm.cpp#L220)

```cpp
        static const std::array<DRMFormat, 6> supportedShmFourccFormats = {
            DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010,
        };

        for (auto const& fmt : supportedShmFourccFormats) {
            m_shmFormats.push_back(fmt);
        }
    }
```

## src/protocols/ColorManagement.cpp:17

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/protocols/ColorManagement.cpp#L17)

```cpp
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES);
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES);
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB);
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
    m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME);

    if (m_resource->version() >= 3)
        m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100);

    if (PROTO::colorManagement->m_debug) {
        m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4);
        m_resource->sendSupportedFeature(WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER);
    }

    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_PAL);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_NTSC);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB);
    m_resource->sendSupportedPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ);

    if (m_resource->version() == 1) {
        m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
        m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_SRGB);
    } else
        m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4);

    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC);
    m_resource->sendSupportedTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428);
```

## src/protocols/ColorManagement.cpp:169

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/protocols/ColorManagement.cpp#L169)

```cpp
    m_resource->setCreateWindowsScrgb([](CWpColorManagerV1* r, uint32_t id) {
        LOGM(Log::WARN, "New Windows scRGB description id={}", id);

        const auto RESOURCE = PROTO::colorManagement->m_imageDescriptions.emplace_back(
            makeShared<CColorManagementImageDescription>(makeShared<CWpImageDescriptionV1>(r->client(), r->version(), id), false));

        if UNLIKELY (!RESOURCE->good()) {
            r->noMemory();
            PROTO::colorManagement->m_imageDescriptions.pop_back();
            return;
        }

        RESOURCE->m_self     = RESOURCE;
        RESOURCE->m_settings = SCRGB_IMAGE_DESCRIPTION;

        RESOURCE->sendMaybeReady();
    });

```

## src/helpers/cm/ColorManagement.hpp:413

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/helpers/cm/ColorManagement.hpp#L413)

```cpp
    inline const auto SCRGB_IMAGE_DESCRIPTION = CImageDescription::from(SImageDescription{
        .transferFunction = NColorManagement::CM_TRANSFER_FUNCTION_EXT_LINEAR,
        .isWindows        = true,
        .primariesNameSet = true,
        .primariesNamed   = NColorManagement::CM_PRIMARIES_SRGB,
        .primaries        = NColorPrimaries::BT709,
        .luminances       = {.reference = 203},
    });

```

## src/render/shaders/glsl/cm_helpers.glsl:148

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/render/shaders/glsl/cm_helpers.glsl#L148)

```cpp
vec4 toLinear(vec4 color, int tf) {
    if (tf == CM_TRANSFER_FUNCTION_EXT_LINEAR)
        return color;

    color.rgb /= max(color.a, 0.001);
    color.rgb = toLinearRGB(color.rgb, tf);
    color.rgb *= color.a;
    return color;
}
```

## src/render/gl/GLTexture.cpp:32

[Pinned source](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/render/gl/GLTexture.cpp#L32)

```cpp
CGLTexture::CGLTexture(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const Vector2D& size_, bool keepDataCopy, bool opaque) :
    ITexture(drmFormat, pixels, stride, size_, keepDataCopy, opaque) {

    g_pHyprOpenGL->makeEGLCurrent();

    const auto format = getPixelFormatFromDRM(drmFormat);
    ASSERT(format);

    m_type          = format->withAlpha ? TEXTURE_RGBA : TEXTURE_RGBX;
    m_size          = size_;
    m_isSynchronous = true;
    m_target        = GL_TEXTURE_2D;
    allocate(size_);
    bind();
    setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (format->swizzle.has_value())
        swizzle(format->swizzle.value());

    bool alignmentChanged = false;
    if (format->bytesPerBlock != 4) {
        const GLint alignment = (stride % 4 == 0) ? 4 : 1;
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, alignment));
        alignmentChanged = true;
    }

    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / format->bytesPerBlock));
    GLCALL(glTexImage2D(GL_TEXTURE_2D, 0, format->glInternalFormat ? format->glInternalFormat : format->glFormat, size_.x, size_.y, 0, format->glFormat, format->glType, pixels));
    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0));
    if (alignmentChanged)
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
```

## src/protocols/core/Shm.cpp:146 format rejection

```cpp
        if UNLIKELY (std::ranges::find(PROTO::shm->m_shmFormats, fmt) == PROTO::shm->m_shmFormats.end()) {
            r->error(WL_SHM_ERROR_INVALID_FORMAT, "Format invalid");
            return;
        }
```

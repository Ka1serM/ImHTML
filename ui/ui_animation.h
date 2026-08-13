#pragma once

#include <memory>
#include <string>

#include "imhtml.hpp"

namespace litehtml {
class element;
}

class IUiAnimation {
public:
    virtual ~IUiAnimation() = default;
    virtual void begin_frame(float delta_seconds) = 0;
    virtual ImHTML::ElementLayerTransform transform(
        const std::shared_ptr<litehtml::element>& element, const ImRect& bounds) = 0;
};

// Default animation provider for the UI library.  It is deliberately exposed
// as an interface: an embedding application may replace it with its own
// provider while the built-in implementation uses ImAnim channels.
class ImAnimUiAnimation final : public IUiAnimation {
public:
    void set_document(const std::string& document, int generation);
    void reset();
    void begin_frame(float delta_seconds) override;
    ImHTML::ElementLayerTransform transform(
        const std::shared_ptr<litehtml::element>& element, const ImRect& bounds) override;

private:
    std::string document_;
    int generation_ = 1;
    float delta_seconds_ = 1.0f / 60.0f;
};


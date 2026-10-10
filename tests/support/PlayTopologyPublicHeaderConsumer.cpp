#include "Horo/Application/PlayTopology.h"

int main() {
    const Horo::Application::PlayTopologyCatalog catalog;
    const auto encoded = Horo::Application::SerializePlayTopologies(catalog);
    return encoded.HasValue() && Horo::Application::ParsePlayTopologies(encoded.Value()).HasValue() ? 0 : 1;
}

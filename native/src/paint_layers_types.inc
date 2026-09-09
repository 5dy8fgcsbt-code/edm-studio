// Included inside PaintCanvas::Impl. Immutable history states share sparse maps and pixel tiles.
static constexpr size_t layerStorageLimit = 2ull * 1024 * 1024 * 1024;
struct LayerMemory {
    size_t bytes = 0;
};
struct LayerTile {
    PaintImage image;
    std::shared_ptr<LayerMemory> memory;
    LayerTile(PaintImage pixels, std::shared_ptr<LayerMemory> owner)
        : image(std::move(pixels)), memory(std::move(owner)) {
        memory->bytes += image.rgba.size();
    }
    ~LayerTile() {
        memory->bytes -= image.rgba.size();
    }
};
using LayerTiles = std::map<uint32_t, std::shared_ptr<LayerTile>>;
struct Layer {
    PaintLayerInfo info;
    std::shared_ptr<LayerTiles> tiles;
};
struct LayerState {
    std::vector<Layer> layers;
    uint64_t active = 0;
};
struct LayerData {
    std::shared_ptr<const PaintImage> base;
    std::shared_ptr<LayerMemory> memory = std::make_shared<LayerMemory>();
    std::shared_ptr<LayerState> state;
};

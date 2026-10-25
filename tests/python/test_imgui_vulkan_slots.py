"""Pinned build-only ImGui adaptation preserves source and output ownership."""
import hashlib

import pytest
import project


def encoder_fixture():
    return ("// retained prefix\n" + project.IMGUI_VULKAN_SIGNATURE + "\n{\n"
            "        return;\n" + project.IMGUI_VULKAN_BUFFER_RING + "\n" + project.IMGUI_VULKAN_BUFFER_GROWTH + "\n"
            "AlignBufferSize(draw_data->TotalVtxCount * sizeof(ImDrawVert), bd->BufferMemoryAlignment)\n"
            "AlignBufferSize(draw_data->TotalIdxCount * sizeof(ImDrawIdx), bd->BufferMemoryAlignment)\n"
            "        check_vk_result(err);\n"
            "        err = vkMapMemory(v->Device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);\n"
            "        check_vk_result(err);\n"
            "        VkMappedMemoryRange range[2] = {};\n        vkUnmapMemory(v->Device, rb->IndexBufferMemory);\n"
            "                VkDescriptorSet desc_set = (VkDescriptorSet)pcmd->GetTexID();\n}\n"
            + project.IMGUI_VULKAN_END_MARKER + "\n{ return true; }\n// retained suffix\n").encode()


def admit_fixture(monkeypatch, source):
    # Synthetic input exercises exact-site rejection; production still requires the fixed upstream digest.
    monkeypatch.setattr(project, "IMGUI_VULKAN_SOURCE_SHA256", hashlib.sha256(source).hexdigest())


def test_encoder_preserves_unrelated_bytes_and_uses_exact_canonical_slot(monkeypatch):
    source = encoder_fixture()
    admit_fixture(monkeypatch, source)
    generated = project.imgui_vulkan_slots_transform(source)
    assert generated.startswith("// retained prefix\n")
    assert generated.endswith(project.IMGUI_VULKAN_END_MARKER + "\n{ return true; }\n// retained suffix\n")
    assert "wrb->Index = canonical_slot;" in generated
    assert "wrb->Index = (wrb->Index + 1)" not in generated
    assert "if (err != VK_SUCCESS)" in generated
    assert "vkUnmapMemory(v->Device, rb->VertexBufferMemory);" in generated
    assert "resolve_texture(pcmd->GetTexID(), texture_context)" in generated


@pytest.mark.parametrize("change", ["duplicate-boundary", "duplicate-ring", "missing-growth"])
def test_encoder_rejects_drift_or_ambiguous_sites(monkeypatch, change):
    source = encoder_fixture()
    if change == "duplicate-boundary":
        source += project.IMGUI_VULKAN_SIGNATURE.encode()
    elif change == "duplicate-ring":
        source = source.replace(project.IMGUI_VULKAN_BUFFER_RING.encode(),
                                (project.IMGUI_VULKAN_BUFFER_RING * 2).encode())
    else:
        source = source.replace(project.IMGUI_VULKAN_BUFFER_GROWTH.encode(), b"changed upstream allocation")
    admit_fixture(monkeypatch, source)
    with pytest.raises(ValueError, match="boundary changed|absent or ambiguous"):
        project.imgui_vulkan_slots_transform(source)


def test_invalid_pinned_source_preserves_existing_output(tmp_path):
    source = tmp_path / "fetched" / "backends" / "imgui.cpp"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"unreviewed upstream bytes")
    output = tmp_path / "build" / "generated.cpp"
    output.parent.mkdir()
    output.write_bytes(b"previous successful output")
    with pytest.raises(ValueError, match="differs from reviewed revision"):
        project.main(["imgui-vulkan-slots", "--source", str(source), "--output", str(output)])
    assert output.read_bytes() == b"previous successful output"
    assert source.read_bytes() == b"unreviewed upstream bytes"


@pytest.mark.parametrize("symlink", [False, True])
def test_output_cannot_escape_into_fetched_tree_after_resolution(tmp_path, symlink):
    fetched = tmp_path / "fetched"
    source = fetched / "backends" / "imgui.cpp"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"unreviewed upstream bytes")
    parent = fetched
    if symlink:
        parent = tmp_path / "alias"
        try:
            parent.symlink_to(fetched, target_is_directory=True)
        except OSError:
            pytest.skip("This host does not permit directory symlinks")
    output = parent / "generated.cpp"
    with pytest.raises(SystemExit) as error:
        project.main(["imgui-vulkan-slots", "--source", str(source), "--output", str(output)])
    assert error.value.code == 2
    assert not (fetched / "generated.cpp").exists()
    assert source.read_bytes() == b"unreviewed upstream bytes"

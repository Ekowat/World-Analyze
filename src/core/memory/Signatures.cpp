#include <worldanalysis/memory/Signatures.hpp>

#include <array>
#include <string>
#include <vector>
#include <pl/memory/Signature.hpp>

namespace worldanalysis::memory {
namespace {
std::array<std::uintptr_t, SignatureCount> addresses{};
// ARM64 dot52 profile. New patterns are exact .text bytes from the supplied ELF.
// PC-relative instructions remain unchanged under ASLR. See compatibility/dot52-bindings.json.
const std::array<SignatureDefinition, SignatureCount> definitions{{
    SignatureDefinition{SignatureId::VersionString, "FF 03 01 D1 FD 7B 02 A9 F4 4F 03 A9 FD 83 00 91 54 D0 3B D5 F3 03 08 AA 88 16 40 F9 A8 83 1F F8 E8 03 00 91 AC FF FF 97 42 84 F9 D0 42 2C 0A 91 E0 03 00 91 E1 03 1F AA 23 00 80 52 FD 5E D8 94"},
    SignatureDefinition{SignatureId::Nametag, "FF 03 07 D1 EB 2B 14 6D E9 23 15 6D FD 7B 16 A9 FC 6F 17 A9 FA 67 18 A9 F8 5F 19 A9 F6 57 1A A9 F4 4F 1B A9 FD 83 05 91 57 D0 3B D5 FA 03 01 AA F5 03 00 AA E9 16 40 F9 F4 03 08 AA A9 03 1D F8"},
    SignatureDefinition{SignatureId::Fullbright, "FD 7B BF A9 FD 03 00 91 08 00 40 F9 41 06 80 52 08 09 40 F9 00 01 3F D6 FD 7B C1 A8 AC 16 77 14 FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 08 00 40 F9 F3 03 01 2A 01 06 80 52 08 71 40 F9 00 01 3F D6"},
    SignatureDefinition{SignatureId::SetupFogPlayer, "FF C3 02 D1 EA 2B 00 FD E9 23 06 6D FD 7B 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD C3 01 91 57 D0 3B D5 F3 03 00 AA E8 16 40 F9 A8 83 1D F8 28 08 40 BD ? ? ? ? 60 82 48 F9 08 00 40 F9"},
    SignatureDefinition{SignatureId::RaknetUpdate, "FD 7B BC A9 FC 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 FF C3 0C D1 55 D0 3B D5 F3 03 00 AA A8 16 40 F9 A8 83 1F F8 14 0C 40 F9 00 80 00 91 ? ? ? ? 88 02 40 F9 01 08 40 A9 E0 03 14 AA"},
    SignatureDefinition{SignatureId::NormalTick, "E8 0F 19 FC FD 7B 01 A9 FC 6F 02 A9 FA 67 03 A9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 43 00 91 FF C3 09 D1 54 D0 3B D5 F3 03 00 AA 88 16 40 F9 A8 03 1E F8 08 C0 78 39 ? ? ? ? 68 92 4C B9"},
    SignatureDefinition{SignatureId::Time, "FF 43 01 D1 FD 7B 02 A9 F5 1B 00 F9 F4 4F 04 A9 FD 83 00 91 55 D0 3B D5 A8 16 40 F9 A8 83 1F F8 08 00 40 F9 08 49 46 F9 00 01 3F D6 08 00 40 F9 09 0D 40 F9 E8 03 00 91 20 01 3F D6 E8 03 40 F9"},
    SignatureDefinition{SignatureId::SetTime, "FF 03 01 D1 FD 7B 02 A9 F4 4F 03 A9 FD 83 00 91 54 D0 3B D5 F3 03 01 2A 88 16 40 F9 A8 83 1F F8 08 00 40 F9 08 4D 46 F9 00 01 3F D6 08 00 40 F9 09 11 40 F9 E8 03 00 91 20 01 3F D6 E8 03 40 F9"},
    SignatureDefinition{SignatureId::EduMultiplayer, "FF C3 01 D1 FD 7B 04 A9 F5 2B 00 F9 F4 4F 06 A9 FD 03 01 91 55 D0 3B D5 F3 03 00 AA A8 16 40 F9 A8 83 1F F8 08 00 40 F9 09 7D 43 F9 E8 83 00 91 20 01 3F D6 E8 13 40 F9 E8 09 00 B4 08 01 40 39"},
    SignatureDefinition{SignatureId::HudCursor, "E9 23 B9 6D FD 7B 01 A9 FC 6F 02 A9 FA 67 03 A9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 43 00 91 FF 43 12 D1 5C D0 3B D5 F4 03 00 AA 00 40 00 91 88 17 40 F9 F6 03 03 AA F3 03 02 AA F5 03 01 AA"},
    SignatureDefinition{SignatureId::LevelInit, "FF 03 07 D1 FD 7B 16 A9 FC 6F 17 A9 FA 67 18 A9 F8 5F 19 A9 F6 57 1A A9 F4 4F 1B A9 FD 83 05 91 48 D0 3B D5 77 C1 01 F0 F7 22 06 91 E8 8F 01 A9 F3 03 00 AA E0 03 17 AA 08 15 40 F9 FC 03 06 AA"},
    SignatureDefinition{SignatureId::LevelDtor, "FF 43 02 D1 FD 7B 05 A9 F8 5F 06 A9 F6 57 07 A9 F4 4F 08 A9 FD 43 01 91 56 D0 3B D5 F3 03 00 AA C8 16 40 F9 A8 83 1F F8 68 A9 01 F0 08 61 08 91 0A 88 41 F9 08 00 00 F9 09 E1 33 91 08 61 35 91"},
    SignatureDefinition{SignatureId::ActorManagerList, "FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 18 0C 40 F9 1F 7D 00 A9 1F 09 00 F9 ? ? ? ? F4 03 00 AA F3 03 08 AA FB 03 1F AA 1A 00 FC 92 ? ? ? ?"},
    SignatureDefinition{SignatureId::DimensionTick, "FF 83 06 D1 EC 8B 00 FD EB 2B 12 6D E9 23 13 6D FD 7B 14 A9 FC 6F 15 A9 FA 67 16 A9 F8 5F 17 A9 F6 57 18 A9 F4 4F 19 A9 FD 03 05 91 5C D0 3B D5"},
    SignatureDefinition{SignatureId::WeatherTick, "FD 7B BE A9 F4 4F 01 A9 FD 03 00 91 F3 03 00 AA 00 2C 40 F9 08 80 43 39 ? ? ? ? 68 32 40 B9 62 86 48 2D 60 3A 40 BD 08 05 00 11 68 32 00 B9"},
    SignatureDefinition{SignatureId::WeatherGetRainLevel, "08 2C 40 F9 08 81 43 39 C8 00 00 34 01 88 46 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E C0 03 5F D6 00 E4 00 2F C0 03 5F D6"},
    SignatureDefinition{SignatureId::WeatherGetLightningLevel, "08 2C 40 F9 08 81 43 39 C8 00 00 34 01 08 48 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E C0 03 5F D6 00 E4 00 2F C0 03 5F D6"},
    SignatureDefinition{SignatureId::WeatherIsRaining, "28 00 80 52 09 2C 40 F9 00 01 22 1E 28 81 43 39 68 01 00 34 01 88 46 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E A8 99 99 52 88 C9 A7 72 01 01 27 1E 10 20 21 1E E0 D7 9F 1A C0 03 5F D6 00 E4 00 2F A8 99 99 52 88 C9 A7 72 01 01 27 1E 10 20 21 1E E0 D7 9F 1A C0 03 5F D6"},
    SignatureDefinition{SignatureId::WeatherIsLightning, "08 2C 40 F9 08 81 43 39 48 02 00 34 00 04 48 2D 28 00 80 52 02 8C 46 2D 04 01 22 1E C8 CC 8C 52 21 38 20 1E C8 EC A7 72 63 38 22 1E 21 08 24 1E 63 08 24 1E 00 28 21 1E 41 28 23 1E 00 08 21 1E 01 01 27 1E 10 20 21 1E E0 D7 9F 1A C0 03 5F D6 E0 03 1F 2A C0 03 5F D6"},
    SignatureDefinition{SignatureId::RenderItem, "EE 0F 16 FC ED 33 01 6D EB 2B 02 6D E9 23 03 6D FD 7B 04 A9 FC 6F 05 A9 FA 67 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 03 01 91 FF 43 0D D1 57 D0 3B D5 E8 16 40 F9 A8 03 1B F8 68 8C 40 39"},
    SignatureDefinition{SignatureId::GetFov, "EA 0F 1C FC E9 A3 00 6D FD FB 01 A9 F5 17 00 F9 F4 4F 03 A9 FD 63 00 91 08 40 20 1E F4 03 01 2A F3 03 00 AA 41 03 00 36 68 92 48 F9 E1 05 80 52 00 DD 41 F9 08 00 40 F9 08 71 40 F9 00 01 3F D6"},
    SignatureDefinition{SignatureId::GetPerspective, "FD 7B BF A9 FD 03 00 91 00 04 40 F9 08 00 40 F9 08 BD 42 F9 00 01 3F D6 08 00 40 F9 01 E5 41 F9 FD 7B C1 A8 20 00 1F D6 FD 7B BF A9 FD 03 00 91 00 04 40 F9 08 00 40 F9 08 81 40 F9 00 01 3F D6"},
    SignatureDefinition{SignatureId::CameraPlayerVisibilityTick, "FF 03 02 D1 FD 7B 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD 03 01 91 57 D0 3B D5 F3 03 00 AA E0 63 00 91 E8 16 40 F9 21 A0 00 91 F4 03 03 AA F5 03 02 AA A8 83 1F F8 ? ? ? ? E0 63 00 91 ? ? ? ? A0 08 00 36"},
    SignatureDefinition{SignatureId::ClientInstanceUpdate, "FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF C3 12 D1 59 D0 3B D5 F3 03 00 AA F4 03 01 2A 28 17 40 F9 A8 83 1F F8 08 00 40 F9 09 39 46 F9 E8 E3 01 91"},
    SignatureDefinition{SignatureId::ClientInstanceGetLocalPlayer, "FF 43 01 D1 FD 7B 03 A9 F3 23 00 F9 FD C3 00 91 53 D0 3B D5 E8 03 00 AA E0 23 00 91 69 16 40 F9 01 A1 08 91 A9 83 1F F8 B4 EA 32 95 E0 23 00 91 B3 EA 32 95 A0 00 00 36 E0 23 00 91 21 00 80 52"},
    SignatureDefinition{SignatureId::ContainerScreenControllerDtor, "FF 83 01 D1 FD 7B 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 43 00 91 56 D0 3B D5 F3 03 00 AA C8 16 40 F9 E8 07 00 F9 E8 8D 04 B0 08 81 1C 91 08 00 00 F9 00 F0 44 F9 08 01 0A 91"},
    SignatureDefinition{SignatureId::ContainerScreenControllerOpen, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA 9C 33 28 94 60 F2 44 F9 E1 03 1F 2A D7 06 20 94 68 02 40 F9 01 D5 40 F9 E0 03 13 AA F3 0B 40 F9 FD 7B C2 A8 20 00 1F D6 3F 04 00 71 61 00 00 54"},
    SignatureDefinition{SignatureId::ChatScreenDtor, "FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 88 8E 04 D0 08 A1 1C 91 F3 03 00 AA 09 01 07 91 08 00 00 F9 09 58 04 F9 00 40 2D 91 AA CF 2A 95 60 B6 45 F9 7F B6 05 F9 80 00 00 B4 08 00 40 F9"},
    SignatureDefinition{SignatureId::ChatScreenOpen, "FF C3 02 D1 FD 7B 08 A9 F6 57 09 A9 F4 4F 0A A9 FD 03 02 91 56 D0 3B D5 F3 03 00 AA C8 16 40 F9 A8 83 1F F8 14 6C 44 F9 ? ? ? ? F5 03 00 AA ? ? ? ? ? ? ? ? E0 43 00 91 A2 03 80 52"},
    SignatureDefinition{SignatureId::BiomeGetTemperature, "FF C3 01 D1 E9 23 01 6D FD 7B 02 A9 F9 1B 00 F9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 83 00 91 57 D0 3B D5 F6 03 00 AA E0 03 01 AA E8 16 40 F9 F3 03 02 AA F4 03 01 AA E8 07 00 F9 28 00 40 F9"},
    SignatureDefinition{SignatureId::GetDestroyProgress, "FF 83 01 D1 FD 7B 02 A9 F7 1B 00 F9 F6 57 04 A9 F4 4F 05 A9 FD 83 00 91 57 D0 3B D5 F4 03 01 AA F3 03 00 AA E8 16 40 F9 A8 83 1F F8 ? ? ? ? E0 07 00 F9 E0 23 00 91 E1 13 00 B9 ? ? ? ?"},
    SignatureDefinition{SignatureId::RenderLevel, "EE 0F 16 FC ED 33 01 6D EB 2B 02 6D E9 23 03 6D FD 7B 04 A9 FC 6F 05 A9 FA 67 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 03 01 91 FF 03 06 D1 57 D0 3B D5 F8 03 00 AA F4 03 02 AA E8 16 40 F9"},
    SignatureDefinition{SignatureId::TessellatorBegin, "FD 7B BC A9 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 08 60 4A 39 09 54 49 39 08 01 09 2A 28 0E 00 35 F3 03 00 AA 1F 90 02 B9 F6 03 04 2A 1F 54 09 39 F4 03 03 2A F5 03 02 2A 1F 60 0A 39"},
    SignatureDefinition{SignatureId::TessellatorColor, "E8 6F A8 52 0C 50 49 39 04 01 27 1E 00 08 24 1E 21 08 24 1E 42 08 24 1E 63 08 24 1E 08 00 38 1E 29 00 38 1E 4A 00 38 1E 6B 00 38 1E EC 02 00 37 6B 7D AB 0A EC 1F 80 52 4A 7D AA 0A 29 7D A9 0A"},
    SignatureDefinition{SignatureId::TessellatorVertex, "FF C3 02 D1 EA 1B 00 FD E9 23 04 6D FD 7B 05 A9 FC 6F 06 A9 FA 67 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD 43 01 91 5A D0 3B D5 48 17 40 F9 E8 17 00 F9 08 90 42 B9 09 94 42 B9 1F 01 09 6B"},
    SignatureDefinition{SignatureId::MeshHelpersRenderMeshImmediately, "FD 7B BB A9 FC 0B 00 F9 F8 5F 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 03 00 91 FF 03 0A D1 58 D0 3B D5 F7 03 00 AA E0 03 01 AA 08 17 40 F9 F4 03 04 AA F5 03 03 AA F6 03 02 AA F3 03 01 AA A8 83 1F F8"},
    SignatureDefinition{SignatureId::MeshHelpersRenderMeshImmediately2, "FD 7B BC A9 FC 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 FF 03 0A D1 57 D0 3B D5 F6 03 00 AA E0 03 01 AA E8 16 40 F9 F4 03 03 AA F5 03 02 AA F3 03 01 AA A8 83 1F F8 13 AB D3 97 C0 02 00 37"},
    SignatureDefinition{SignatureId::RenderMaterialGroupCommon, "E0 CF 04 90 00 A0 13 91 A1 83 00 D1 A2 43 01 D1 E4 83 01 91 E3 03 14 AA 25 00 80 52 13 A4 3B 94 F4 37 40 F9 FF 7F 06 A9 F4 06 00 B4 81 22 00 91 00 00 80 92 87 BA AE 97 E0 00 00 B5 88 02 40 F9"},
    SignatureDefinition{SignatureId::SurvivalModeAttack, "08 20 43 39 E3 03 02 AA ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? E0 03 1F 2A C0 03 5F D6 22 00 80 52 ? ? ? ? 08 20 43 39 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? E0 03 1F 2A"},
    SignatureDefinition{SignatureId::GameModeAttack, "FF 43 07 D1 FD 7B 18 A9 FC CB 00 F9 F8 5F 1A A9 F6 57 1B A9 F4 4F 1C A9 FD 03 06 91 58 D0 3B D5 F3 03 00 AA F4 03 03 AA 08 17 40 F9 F5 03 02 2A F6 03 01 AA A8 83 1F F8 00 04 40 F9 2E 2D F3 97"},
    SignatureDefinition{SignatureId::LevelGetHitResult, "00 E8 40 F9 C0 03 5F D6 00 E8 40 F9 ? ? ? ? FD 7B BD A9 F6 57 01 A9 F4 4F 02 A9 FD 03 00 91 09 E4 40 F9 1F 05 00 F9 ? ? ? ? 35 D9 40 A9 F3 03 00 AA F4 03 08 AA ? ? ? ? C1 22 00 91"},
    // I18nImpl singleton accessor recovered from the supplied 26.45 target binary.
    SignatureDefinition{SignatureId::I18nGetInstance, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 28 68 03 90 08 41 32 91 08 FD DF 08 C8 00 00 36 20 68 03 90 00 A0 24 91 F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6 20 68 03 90 00 40 32 91 D9 72 89 95 00 FF FF 34"},
    SignatureDefinition{SignatureId::BlockSourceGetBiome, "FF 03 01 D1 FD 7B 01 A9 F5 13 00 F9 F4 4F 03 A9 FD 43 00 91 55 D0 3B D5 F3 03 00 AA F4 03 01 AA A8 16 40 F9 E8 07 00 F9 08 00 40 F9 08 9D 40 F9 00 01 3F D6 ? ? ? ? 89 06 40 B9 68 86 C0 79"},
    SignatureDefinition{SignatureId::BlockSourceGetBlock, "FF 03 01 D1 FD 7B 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 43 00 91 56 D0 3B D5 C8 16 40 F9 E8 07 00 F9 28 04 40 B9 09 84 C0 79 1F 01 09 6B ? ? ? ? 09 80 C0 79 F3 03 00 AA 1F 01 09 6B ? ? ? ?"},
    SignatureDefinition{SignatureId::BlockSourceGetBrightness, "FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 F3 03 00 AA 00 1C 40 F9 F4 03 01 AA 08 00 40 F9 08 A9 40 F9 00 01 3F D6 68 1E 40 F9 F5 03 00 AA E0 03 08 AA ? ? ? ? 02 1C 40 92 E1 03 14 AA"},
    SignatureDefinition{SignatureId::BlockSourceIsSolidBlockingBlock, "09 00 40 F9 23 08 40 B9 28 08 40 29 24 DD 40 F9 E1 03 08 2A 80 00 1F D6 FF 03 01 D1 FD 7B 02 A9 F3 1B 00 F9 FD 83 00 91 53 D0 3B D5 68 16 40 F9 A8 83 1F F8 08 00 40 F9 20 00 40 FD 29 08 40 B9"},
    SignatureDefinition{SignatureId::NavigationComponentGetPath, "00 2C 40 F9 C0 03 5F D6 FD 7B BE A9 F3 0B 00 F9"},
    // NavigationComponent::setPath(unique_ptr<Path>) in Minecraft 26.45.
    // Unique in dot45values.so; swaps the incoming unique_ptr into mPath at +0x58.
    SignatureDefinition{SignatureId::NavigationComponentSetPath, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 28 00 40 F9 3F 00 00 F9 13 2C 40 F9 08 2C 00 F9 F3 00 00 B4 E0 03 13 AA"},
    SignatureDefinition{SignatureId::PathGetNodeCount, "09 20 40 A9 08 01 09 CB 00 FD 44 93 C0 03 5F D6 09 20 40 A9"},
    SignatureDefinition{SignatureId::PathGetCurrentIndex, "00 0C 40 F9 C0 03 5F D6 01 0C 00 F9 C0 03 5F D6 EA 0F 1C FC"},
    SignatureDefinition{SignatureId::PathGetPosAtNode, "EA 0F 1C FC E9 23 01 6D FD 7B 02 A9 F4 4F 03 A9 FD 83 00 91 08 24 40 A9 29 01 08 EB 20 04 00 54"},
    SignatureDefinition{SignatureId::PathNavigationAssignPath, "FF 43 01 D1 E8 0B 00 FD FD FB 01 A9 F7 17 00 F9 F6 57 03 A9 F4 4F 04 A9 FD 63 00 91 57 D0 3B D5 E8 16 40 F9 E8 07 00 F9 68 00 40 F9"},
    // Minecraft 26.45 PathNavigation::tick implementations. These are the common owner-aware
    // capture points for the native navigation families used by mobs in this target binary.
    SignatureDefinition{SignatureId::PathNavigationTickBase, "FF 03 04 D1 EA 43 00 FD E9 23 09 6D FD 7B 0A A9 FC 6F 0B A9 FA 67 0C A9 F8 5F 0D A9 F6 57 0E A9 F4 4F 0F A9 FD 83 02 91 57 D0 3B D5 F5 03 00 AA E0 03 01 AA E8 16 40 F9 F3 03 02 AA F4 03 01 AA"},
    SignatureDefinition{SignatureId::PathNavigationTickGeneric, "FF 83 03 D1 EE 2B 00 FD ED 33 06 6D EB 2B 07 6D E9 23 08 6D FD 7B 09 A9"},
    SignatureDefinition{SignatureId::PathNavigationTickFlying, "FF 03 02 D1 EA 13 00 FD E9 A3 02 6D FD FB 03 A9 F9 27 00 F9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD E3 00 91 58 D0 3B D5 F5 03 00 AA E0 03 01 AA"},
    SignatureDefinition{SignatureId::PathNavigationTickHover, "FF 43 02 D1 EA 1B 00 FD E9 23 04 6D FD 7B 05 A9 F8 5F 06 A9 F6 57 07 A9 F4 4F 08 A9 FD 43 01 91 58 D0 3B D5 F6 03 00 AA"},
    SignatureDefinition{SignatureId::PathNavigationTickWallClimber, "FF C3 02 D1 EA 1B 00 FD E9 23 04 6D FD 7B 05 A9 FC 6F 06 A9 FA 67 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD 43 01 91 58 D0 3B D5 99 48 89 52"},
    SignatureDefinition{SignatureId::PathNavigationTickWaterBound, "FF 83 02 D1 EB 2B 04 6D E9 23 05 6D FD 7B 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 83 01 91 57 D0 3B D5 F5 03 00 AA E0 03 01 AA E8 16 40 F9 F3 03 02 AA F4 03 01 AA A8 83 1D F8 FD 89 2C 97"},
    SignatureDefinition{SignatureId::CameraStateApply, "00 80 C0 3C 28 50 04 91 01 E4 00 2F 5F 00 00 72 00 01 80 3D 00 80 C7 3C 20 4C 80 3D 00 18 40 BD 00 1C 21 1E 20 20 01 BD 00 88 40 BD 00 1C 21 1E"},
    SignatureDefinition{SignatureId::LocalPlayerApplyTurnDelta, "FF C3 02 D1 E8 33 00 FD FD 7B 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD C3 01 91 56 D0 3B D5 F3 03 00 AA F4 03 01 AA C8 16 40 F9 A8 83 1E F8 00 44 46 F9 08 00 40 F9 09 01 43 F9 E8 83 00 91"},
    SignatureDefinition{SignatureId::BaseOptionRegistryGetHideItemInHand, "FD 7B BF A9 FD 03 00 91 08 00 40 F9 C1 05 80 52 08 09 40 F9 00 01 3F D6 FD 7B C1 A8 AA 11 77 14 FD 7B BF A9 FD 03 00 91 08 00 40 F9 01 3D 80 52 08 09 40 F9 00 01 3F D6 60 00 00 B4 FD 7B C1 A8"},
    SignatureDefinition{SignatureId::HitResultGetEntity, "FF 43 01 D1 FD 7B 03 A9 F3 23 00 F9 FD C3 00 91 53 D0 3B D5 E8 03 00 AA E0 23 00 91 69 16 40 F9 01 E1 00 91 A9 83 1F F8 ? ? ? ? E0 23 00 91 ? ? ? ? ? ? ? ? E0 23 00 91 21 00 80 52"},
    SignatureDefinition{SignatureId::ActorSetPos, "FD 7B BE A9 F4 4F 01 A9 FD 03 00 91 F3 03 01 AA F4 03 00 AA 41 6F F9 F0 21 FC 0C 91 E0 03 13 AA EC 40 F6 97 88 06 41 F9 69 02 40 F9 6A 0A 40 B9 09 01 00 F9 0A 09 00 B9 F4 4F 41 A9 FD 7B C2 A8"},
    SignatureDefinition{SignatureId::ActorSetPosDelta, "08 04 41 F9 29 08 40 B9 2A 00 40 F9 09 21 00 B9 0A 0D 00 F9 C0 03 5F D6 FD 7B BE A9 F4 4F 01 A9"},
    SignatureDefinition{SignatureId::ActorIsPlayer, "09 08 40 F9 EA B1 84 52 AA 2B BF 72 2B A1 43 A9 08 01 0B CB 08 FD 43 D3 08 05 00 51 0C 01 0A 8A 08 18 40 B9 6B 0D 0C 8B 2C 29 40 F9 6B 01 40 F9 7F 05 00 B1 ? ? ? ? 8B 15 0B 8B 6D 09 40 B9 BF 01 0A 6B ? ? ? ? 29 2D 40 F9 7F 01 09 EB ? ? ? ? 69 09 40 F9 ? ? ? ? 29 AD 40 A9 0A 45 4B D3 6B 01 09 CB 5F 0D 8B EB ? ? ? ? 29 79 6A F8 ? ? ? ? 0A 45 40 92 08 35 0E 12 4A 29 40 92 29 79 6A B8 28 01 08 4A 89 FF BF 12 1F 01 09 6B E0 27 9F 1A C0 03 5F D6 E0 03 1F 2A C0 03 5F D6 FF 43 01 D1 E8 0B 00 FD FD 7B 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 83 00 91 56 D0 3B D5"},
    SignatureDefinition{SignatureId::ActorIsInvisible, "00 20 00 91 A1 00 80 52 ? ? ? ? FF 83 01 D1 FD 7B 04 A9 F4 4F 05 A9 FD 03 01 91 54 D0 3B D5 88 16 40 F9 A8 83 1F F8 08 20 4B 39 09 68 41 F9 0A FD 41 D3 1F 01 00 72 48 01 89 9A ? ? ? ?"},
    // query.health/query.max_health call these two Actor wrappers in this exact 26.45 binary.
    SignatureDefinition{SignatureId::ActorGetHealth, "00 20 00 91 6B 05 11 14"},
    SignatureDefinition{SignatureId::ActorGetMaxHealth, "00 20 00 91 9A AB 10 14"},
    // ActorHasTargetTest uses this resolver: it loads Actor+0x278 target ID and resolves it through Level.
    SignatureDefinition{SignatureId::ActorGetTarget, "E8 03 00 AA 00 E8 40 F9 00 01 00 B4 01 3D 41 F9 3F 04 00 B1 A0 00 00 54 08 00 40 F9 E2 03 1F 2A"},
    // AttributesComponent attribute lookup by canonical string. This 40-byte form is unique in dot45values.so.
    SignatureDefinition{SignatureId::AttributesGetInstanceByName, "FF C3 01 D1 FD 7B 05 A9 F4 4F 06 A9 FD 43 01 91 54 D0 3B D5 F3 03 00 AA E0 03 01 AA 88 16 40 F9 A8 83 1F F8 ED FE FF 97"},
    // Actor equipment accessor: slots 0/1 are hands, slots 2..6 are equippable/armor slots.
    SignatureDefinition{SignatureId::ActorGetEquipmentItem, "3F 04 00 71 A8 01 00 54 FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 00 20 00 91 F3 03 01 2A BA BB 1C 94 08 00 40 F9 02 21 40 F9 E1 03 13 2A F3 0B 40 F9 FD 7B C2 A8 40 00 1F D6 21 08 00 51 3F 10 00 71"},
    SignatureDefinition{SignatureId::ActorFetchNearbyActorsSorted, "FF 83 03 D1 FD 7B 08 A9 FC 6F 09 A9 FA 67 0A A9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 03 02 91 5B D0 3B D5 F3 03 08 AA F5 03 02 2A 68 17 40 F9 F4 03 00 AA E2 A3 00 91 A8 83 1F F8 08 08 41 F9"},
    SignatureDefinition{SignatureId::ActorGetNameTag, "FF C3 01 D1 FD 7B 04 A9 F5 2B 00 F9 F4 4F 06 A9 FD 03 01 91 55 D0 3B D5 F3 03 08 AA F4 03 00 AA A8 16 40 F9 A8 83 1F F8 E8 03 00 91 ? ? ? ? E0 03 00 91 ? ? ? ? 88 22 4B 39 ? ? ? ?"},
    SignatureDefinition{SignatureId::ActorSetNameTag, "FF 83 01 D1 FD 7B 03 A9 F6 57 04 A9 F4 4F 05 A9 FD C3 00 91 56 D0 3B D5 F3 03 00 AA 00 80 04 91 C8 16 40 F9 F4 03 01 AA A8 83 1F F8 25 72 F4 97 81 00 80 52 F5 03 00 AA EA 6F F4 97 E1 03 00 AA"},
    SignatureDefinition{SignatureId::MinecraftUIRenderContextDrawText, "FF C3 00 D1 FD 7B 01 A9 F3 13 00 F9 FD 43 00 91 09 A8 48 A9 E8 03 05 2A F3 03 00 AA 3F 01 0A EB ? ? ? ? CA 10 40 79 21 01 00 F9 20 61 01 91 41 00 C0 3D EB 00 40 B9 C2 00 40 FD 21 81 80 3C"},
    SignatureDefinition{SignatureId::ScreenViewRender, "EC 0F 17 FC EB 2B 01 6D E9 23 02 6D FD 7B 03 A9 FC 6F 04 A9 FA 67 05 A9 F8 5F 06 A9 F6 57 07 A9 F4 4F 08 A9 FD C3 00 91 FF 03 09 D1 48 D0 3B D5 FC 03 00 AA E8 0F 00 F9 08 15 40 F9 A8 83 1B F8"},
    SignatureDefinition{SignatureId::ContainerScreenControllerOnContainerSlotSelected, "FD 7B BF A9 FD 03 00 91 08 00 40 F9 08 E9 40 F9 00 01 3F D6 E0 03 1F 2A FD 7B C1 A8 C0 03 5F D6 FF C3 01 D1 FD 7B 03 A9 F7 23 00 F9 F6 57 05 A9 F4 4F 06 A9 FD C3 00 91 57 D0 3B D5 E8 16 40 F9"},
    SignatureDefinition{SignatureId::ContainerScreenControllerGetItemStack, "FF C3 01 D1 FD 7B 05 A9 F4 4F 06 A9 FD 43 01 91 54 D0 3B D5 F3 03 00 AA E0 23 00 91 88 16 40 F9 A8 83 1F F8 0F E0 7D 95 68 02 40 F9 09 F1 40 F9 E8 A3 00 91 E1 23 00 91 E0 03 13 AA 20 01 3F D6"},
    SignatureDefinition{SignatureId::ContainerManagerControllerGetItemInstance, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 00 A0 00 91 F3 03 02 2A 65 BE 51 96 E0 00 00 B4 00 14 40 F9 A0 00 00 B4 E1 03 13 2A F3 0B 40 F9 FD 7B C2 A8 34 64 01 14 00 DD 01 90 00 00 2B 91 F3 0B 40 F9"},
    SignatureDefinition{SignatureId::ClientNetworkHandlerHandleSetTitle, "FF C3 04 D1 FD 7B 0E A9 FC 7B 00 F9 F8 5F 10 A9 F6 57 11 A9 F4 4F 12 A9 FD 83 03 91 55 D0 3B D5 F4 03 00 AA F3 03 02 AA A8 16 40 F9 A8 83 1F F8 00 2C 40 F9 08 00 40 F9 08 15 46 F9 00 01 3F D6"},
    SignatureDefinition{SignatureId::ClientNetworkHandlerHandleText, "FF 83 05 D1 FD 7B 12 A9 FC 5F 13 A9 F6 57 14 A9 F4 4F 15 A9 FD 83 04 91 57 D0 3B D5 F4 03 00 AA 40 C0 00 91 E8 16 40 F9 F3 03 02 AA A8 83 1F F8 30 D4 C4 94 08 1C 00 12 1F 05 00 71 01 03 00 54"},
    SignatureDefinition{SignatureId::LoopbackPacketSenderSendToServer, "FD 7B BB A9 FC 0B 00 F9 F8 5F 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 03 00 91 FF C3 08 D1 55 D0 3B D5 F4 03 00 AA F3 03 01 AA A8 16 40 F9 A8 83 1F F8 08 60 40 39 09 28 40 B9 28 40 00 39 ? ? ? ?"},
    SignatureDefinition{SignatureId::ClientInstanceGetPacketSender, "00 D8 40 F9 C0 03 5F D6 00 C4 40 F9 08 00 40 F9 01 F1 40 F9 20 00 1F D6 00 C4 40 F9 08 00 40 F9 01 F1 40 F9 20 00 1F D6 FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 09 00 40 F9 F3 03 08 AA 29 8D 43 F9"},
    SignatureDefinition{SignatureId::MinecraftPacketsCreatePacket, "1F 84 05 71 08 3B 00 54 E9 03 00 2A AA 49 FB F0 4A A9 13 91 8B 00 00 10 4C 79 69 78 6B 09 0C 8B 60 01 1F D6 1F 7D 00 A9 C0 03 5F D6 E0 03 08 AA C3 20 00 14 E0 03 08 AA 9D 26 00 14 E0 03 08 AA"},
    SignatureDefinition{SignatureId::LocalPlayerChangeDimension, "FF C3 03 D1 FD 7B 09 A9 FC 6F 0A A9 FA 67 0B A9 F8 5F 0C A9 F6 57 0D A9 F4 4F 0E A9 FD 43 02 91 5A D0 3B D5 F6 03 01 AA F3 03 00 AA 48 17 40 F9 A8 83 1F F8 85 43 2A 95 D4 2E 40 B9 F5 03 00 2A"},
    SignatureDefinition{SignatureId::NbtTreeFind, "FD 7B BC A9 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 F3 03 00 AA 77 8E 40 F8 77 08 00 B4 34 58 40 A9 F5 03 13 AA 04 00 00 14 F5 03 17 AA F7 6A 69 F8 37 05 00 B4 E8 82 40 39 28 02 00 36 E1 1A 40 F9 28 02 00 36 F8 16 40 F9 1F 03 16 EB E0 03 14 AA 02 33 96 9A C1 DD FF 95 20 02 00 34"},
    SignatureDefinition{SignatureId::ItemStackBaseLoadItem, "FF 83 03 D1 FD 7B 08 A9 FC 6F 09 A9 FA 67 0A A9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 03 02 91 5B D0 3B D5 F4 03 01 AA F3 03 00 AA 68 17 40 F9 A8 83 1F F8 E8 C3 00 91 ? ? ? ? ? ? ? ?"},
    // ItemStackBase current damage getter. The previous pattern pointed at an unrelated
    // component routine and was unsafe when called as int(ItemStackBase const*). This exact
    // 26.45 helper follows the stack item/shared counter and Damage component, returning 0
    // for non-damageable/empty stacks. Unique in the supplied dot45values.so.
    SignatureDefinition{SignatureId::ItemStackBaseGetDamageValue, "08 04 40 F9 28 01 00 B4 08 01 40 F9 E8 00 00 B4 00 08 40 F9 A0 00 00 B4 81 9D F9 F0 21 44 38 91 C2 00 80 52 55 82 49 14 E0 03 1F 2A C0 03 5F D6 08 04 40 F9 28 01 00 B4 08 01 40 F9 E8 00 00 B4"},
    // ItemStackBase::getItem() const. Unique in dot45values.so; follows mItem WeakPtr and returns Item* or nullptr.
    SignatureDefinition{SignatureId::ItemStackBaseGetItem, "08 04 40 F9 68 00 00 B4 00 01 40 F9 C0 03 5F D6 E0 03 1F AA C0 03 5F D6"},
    // Legacy ItemStackBase id/aux-style accessor recovered from the 26.45 binary. Do not use this as item identity.
    SignatureDefinition{SignatureId::ItemStackBaseGetId, "09 0C 42 78 E8 FF 8F 52 0A 80 5F F8 3F 01 08 6B 44 19 40 FA 48 A1 04 91 08 00 88 9A 00 01 40 79 C0 03 5F D6"},
    SignatureDefinition{SignatureId::ItemActorClientInitialize, "FF C3 01 D1 FD 7B 02 A9 FA 67 03 A9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 83 00 91 5A D0 3B D5 F8 03 00 AA E0 03 01 AA 48 17 40 F9 F5 03 06 2A F7 03 05 2A F4 03 04 2A F9 03 01 AA F6 03 03 AA"},
    SignatureDefinition{SignatureId::ItemActorPostNormalTick, "FF 83 03 D1 E8 3B 00 FD FD 7B 08 A9 FC 6F 09 A9 FA 67 0A A9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 03 02 91 5A D0 3B D5 F3 03 00 AA 48 17 40 F9 A8 83 1E F8 09 2C 44 B9 28 05 00 71 AB 00 00 54"},
    SignatureDefinition{SignatureId::ItemActorTryGetFromEntity, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 09 04 40 F9 0A 00 87 52 2A B7 B0 72 2B A1 43 A9 08 01 0B CB 08 FD 43 D3 08 05 00 51 0C 01 0A 8A 08 10 40 B9 6B 0D 0C 8B 2C 29 40 F9 6B 01 40 F9 7F 05 00 B1 00 05 00 54 8B 15 0B 8B"},
    SignatureDefinition{SignatureId::BaseActorRenderContextCtor, "FF 43 01 D1 FD 7B 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 83 00 91 56 D0 3B D5 F3 03 00 AA F4 03 02 AA C8 16 40 F9 F5 03 01 AA A8 83 1F F8 ? ? ? ? ? ? ? ? 08 7C 00 A9 1F 08 01 A9 48 00 40 F9"},
    SignatureDefinition{SignatureId::ItemRendererRenderGuiItemNew, "FF C3 05 D1 EC 73 00 FD EB 2B 0F 6D E9 23 10 6D FD 7B 11 A9 FC 6F 12 A9 FA 67 13 A9 F8 5F 14 A9 F6 57 15 A9 F4 4F 16 A9 FD 43 04 91 5B D0 3B D5 F4 03 00 AA E0 03 02 AA 68 17 40 F9 F6 03 05 2A"},
    // MinecraftUIRenderContext's complete item helper. Unlike a raw
    // ItemRenderer::renderGuiItemNew call, this 26.45 wrapper constructs and
    // destroys BaseActorRenderContext, submits the item pass, and performs the
    // enchanted/overlay follow-up required for the icon to become visible.
    SignatureDefinition{SignatureId::MinecraftUIRenderContextRenderItem, "FF 03 05 D1 EA 6B 00 FD E9 23 0E 6D FD 7B 0F A9 FC 67 10 A9 F8 5F 11 A9 F6 57 12 A9 F4 4F 13 A9 FD C3 03 91 59 D0 3B D5 F3 03 00 AA E0 03 02 AA 28 17 40 F9 F4 03 03 2A 48 40 20 1E 29 40 20 1E 0A 40 20 1E F5 03 02 AA F7 03 01 AA A8 83 1D F8"},
    SignatureDefinition{SignatureId::ControlOptionEditorTick, "FF 83 05 D1 EF 3B 0C 6D ED 33 0D 6D EB 2B 0E 6D E9 23 0F 6D FD 7B 10 A9 FC 6F 11 A9 FA 67 12 A9 F8 5F 13 A9 F6 57 14 A9 F4 4F 15 A9 FD 03 04 91 5B D0 3B D5 F4 03 00 AA F8 03 02 AA 68 17 40 F9"},
    SignatureDefinition{SignatureId::ControlOptionEditorRender, "FF C3 03 D1 EE 3B 00 FD ED B3 07 6D EB AB 08 6D E9 A3 09 6D FD FB 0A A9 F9 5F 00 F9 F8 5F 0C A9 F6 57 0D A9 F4 4F 0E A9 FD A3 02 91 55 D0 3B D5 F4 03 00 AA A8 16 40 F9 A8 03 1C F8 00 58 40 F9"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceDown, "FF 83 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 FC 67 1A A9 F8 5F 1B A9 F6 57 1C A9 F4 4F 1D A9 FD 43 06 91 57 D0 3B D5 0A 60 00 91 F6 03 03 AA E8 16 40 F9 F5 03 02 AA F4 03 00 AA F3 03 01 AA A8 83 1B F8 08 C4 41 39 09 20 58 39 1F 01 00 71 98 00 8A 9A 69 02 00 37 88 CE 41 39 08 01 00 34 81 06 40 F9 C1 00 00 B4 80 0A 43 F9 82 62 18 91 83 C2 17 91 EA 18 6E 95"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceUp, "FF 83 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 FC 67 1A A9 F8 5F 1B A9 F6 57 1C A9 F4 4F 1D A9 FD 43 06 91 57 D0 3B D5 0A 60 00 91 F6 03 03 AA E8 16 40 F9 F5 03 02 AA F4 03 00 AA F3 03 01 AA A8 83 1B F8 08 C4 41 39 09 20 58 39 1F 01 00 71 98 00 8A 9A 69 02 00 37 88 CE 41 39 08 01 00 34 81 06 40 F9 C1 00 00 B4 80 0A 43 F9 82 62 18 91 83 C2 17 91 44 16 6E 95"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceNorth, "FF C3 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 88 22 18 39 E0 03 13 AA 41 00 80 52"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceSouth, "FF C3 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 88 22 18 39 E0 03 13 AA 61 00 80 52"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceWest, "FF C3 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 88 22 18 39 E0 03 13 AA 81 00 80 52"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellateFaceEast, "FF C3 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 88 22 18 39 E0 03 13 AA A1 00 80 52"},
    SignatureDefinition{SignatureId::BlockTessellatorTessellatePane, "EB 2B B8 6D E9 23 01 6D FD 7B 02 A9 FC 6F 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD 83 00 91 FF 03 11 D1 5A D0 3B D5 F6 03 00 AA E0 03 02 AA 48 17 40 F9 F3 03 03 AA F4 03 02 AA"},
    SignatureDefinition{SignatureId::BlockSourceGetBlockForTessellation, "5F 04 00 71 A0 00 00 54 E2 00 00 35 08 00 40 F9 02 09 40 F9 40 00 1F D6 08 00 40 F9 02 15 40 F9"},
    SignatureDefinition{SignatureId::TextureUVCoordinateSetCopyCtor, "FD 7B BE A9 F4 4F 01 A9 FD 03 00 91 20 C0 C0 3C 21 00 C0 3D F4 03 01 AA F3 03 00 AA 00 C0 80 3C"},
    SignatureDefinition{SignatureId::TextureUVCoordinateSetDtor, "08 A0 40 39 48 00 00 37 C0 03 5F D6 00 1C 40 F9 ? ? ? ? ? ? ? ? ? ? ? ? 0A 05 00 B4 40 01 67 9E E8 03 00 2A 00 58 20 0E 00 38 30 2E 09 00 26 1E 3F 05 00 F1 88 00 00 54 4B 05 00 51 6B 01 08 8A"},
    SignatureDefinition{SignatureId::RenderChunkCoordinatorSetAllDirty, "FF 03 02 D1 FD 7B 02 A9 FC 6F 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD 83 00 91 48 D0 3B D5 F4 03 02 2A"},
    // DataDrivenModel::render. Hooked after the native render so Determine can
    // consume the exact current-frame BoneOrientation matrices generated by the
    // supplied 26.45 client instead of guessing entity body-part boxes.
    SignatureDefinition{SignatureId::DataDrivenModelRender, "EF 3B B6 6D ED 33 01 6D EB 2B 02 6D E9 23 03 6D FD 7B 04 A9 FC 6F 05 A9 FA 67 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 03 01 91 FF 43 07 D1 E3 93 03 A9 57 D0 3B D5 F4 03 00 AA E8 16 40 F9"},
}};
}

bool resolveAll(std::string_view libraryName) {
    const std::string library(libraryName);
    std::vector<std::string> patterns;
    patterns.reserve(definitions.size());
    for (const auto& definition : definitions) patterns.emplace_back(definition.pattern);
    const auto resolved = pl::memory::resolveSignatures(patterns, library.c_str());
    addresses.fill(0);
    bool any = false;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto it = resolved.find(patterns[i]);
        if (it == resolved.end() || it->second == 0) continue;
        addresses[static_cast<std::size_t>(definitions[i].id)] = it->second;
        any = true;
    }
    return any;
}

std::uintptr_t resolve(SignatureId id) {
    const auto index = static_cast<std::size_t>(id);
    return index < addresses.size() ? addresses[index] : 0;
}

void clear() {
    addresses.fill(0);
}

}

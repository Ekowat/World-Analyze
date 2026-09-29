#pragma once

#include "../Module.hpp"

namespace worldanalysis::sdk { class Player; }

class TechVFXModule : public Module {
public:
    TechVFXModule();
    ~TechVFXModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // A deliberately small global control surface. Individual effects are
    // toggles only; timing/weight jitter is internal so the field never falls
    // into synchronized spawn/expiry waves.
    float spawnRate = 9.0f;
    int maxActive = 84;
    float effectRange = 288.0f;
    float sizeScale = 1.0f;
    float animationSpeed = 1.0f;
    float giantCooldown = 75.0f;

#define TECH_VFX_TOGGLE(name) bool name = true;
    TECH_VFX_TOGGLE(surfaceTrace)
    TECH_VFX_TOGGLE(cornerBrackets)
    TECH_VFX_TOGGLE(scanGrid)
    TECH_VFX_TOGGLE(nodeNetwork)
    TECH_VFX_TOGGLE(microGraph)
    TECH_VFX_TOGGLE(spectrumBars)
    TECH_VFX_TOGGLE(hexPulse)
    TECH_VFX_TOGGLE(crosshairLock)
    TECH_VFX_TOGGLE(dataCascade)
    TECH_VFX_TOGGLE(circuitBranches)
    TECH_VFX_TOGGLE(rulerTicks)
    TECH_VFX_TOGGLE(coordinateStack)
    TECH_VFX_TOGGLE(binaryRain)
    TECH_VFX_TOGGLE(radarSweep)
    TECH_VFX_TOGGLE(concentricRings)
    TECH_VFX_TOGGLE(orbitNodes)
    TECH_VFX_TOGGLE(waveRibbon)
    TECH_VFX_TOGGLE(pulseColumn)
    TECH_VFX_TOGGLE(holoCube)
    TECH_VFX_TOGGLE(holoPyramid)
    TECH_VFX_TOGGLE(wireSphere)
    TECH_VFX_TOGGLE(helix)
    TECH_VFX_TOGGLE(dnaChain)
    TECH_VFX_TOGGLE(arcGauge)
    TECH_VFX_TOGGLE(loadBars)
    TECH_VFX_TOGGLE(matrixPanel)
    TECH_VFX_TOGGLE(triangleFan)
    TECH_VFX_TOGGLE(reticleBurst)
    TECH_VFX_TOGGLE(horizonTicks)
    TECH_VFX_TOGGLE(floatingTerminal)
    TECH_VFX_TOGGLE(skyGraph)
    TECH_VFX_TOGGLE(skyLattice)
    TECH_VFX_TOGGLE(skyArc)
    TECH_VFX_TOGGLE(beaconSpiral)
    TECH_VFX_TOGGLE(dataConstellation)
    TECH_VFX_TOGGLE(packetStream)
    TECH_VFX_TOGGLE(voxelBracket)
    TECH_VFX_TOGGLE(sectorMap)
    TECH_VFX_TOGGLE(diagnosticPanel)
    TECH_VFX_TOGGLE(orbitalBands)
    TECH_VFX_TOGGLE(giantWorldSphere)
    TECH_VFX_TOGGLE(worldAxisBurst)
    TECH_VFX_TOGGLE(squarePulse)
    TECH_VFX_TOGGLE(squareTunnel)
    TECH_VFX_TOGGLE(squareRadar)
    TECH_VFX_TOGGLE(squareMatrix)
    TECH_VFX_TOGGLE(squareStack)
    TECH_VFX_TOGGLE(squareSweep)
    TECH_VFX_TOGGLE(squareCorners)
    TECH_VFX_TOGGLE(squareCrossGrid)
    TECH_VFX_TOGGLE(squareOrbit)
    TECH_VFX_TOGGLE(squareWave)
    TECH_VFX_TOGGLE(squareCodePanel)
    TECH_VFX_TOGGLE(squareHistogram)
    TECH_VFX_TOGGLE(squareNodeFrame)
    TECH_VFX_TOGGLE(squareCircuitMap)
    TECH_VFX_TOGGLE(squareReticle)
    TECH_VFX_TOGGLE(squareTicker)
    TECH_VFX_TOGGLE(squareBarcode)
    TECH_VFX_TOGGLE(squareDiagnostic)
    TECH_VFX_TOGGLE(squareSignalMap)
    TECH_VFX_TOGGLE(squareCompass)
    TECH_VFX_TOGGLE(squareFlowMap)
    TECH_VFX_TOGGLE(squareTimeline)
    TECH_VFX_TOGGLE(squareScope)
    TECH_VFX_TOGGLE(squareProfiler)
    TECH_VFX_TOGGLE(squarePacketGrid)
    TECH_VFX_TOGGLE(squareMemoryMap)
    TECH_VFX_TOGGLE(squareClock)
    TECH_VFX_TOGGLE(squareVectorField)
    TECH_VFX_TOGGLE(squareHeatmap)
    TECH_VFX_TOGGLE(squareTargetArray)
    TECH_VFX_TOGGLE(squareDataWindow)
    TECH_VFX_TOGGLE(squareFragmentField)
    TECH_VFX_TOGGLE(squarePortal)
    TECH_VFX_TOGGLE(squareRain)
    TECH_VFX_TOGGLE(squareScanline)
    TECH_VFX_TOGGLE(squareEqualizer)
    TECH_VFX_TOGGLE(squareWaveform)
    TECH_VFX_TOGGLE(squareTree)
    TECH_VFX_TOGGLE(squareMesh)
    TECH_VFX_TOGGLE(squareLockGrid)
    TECH_VFX_TOGGLE(skyBillboardGrid)
    TECH_VFX_TOGGLE(skySquareArray)
    TECH_VFX_TOGGLE(skyDataWall)
    TECH_VFX_TOGGLE(skyScanPlane)
    TECH_VFX_TOGGLE(skyTimeline)
    TECH_VFX_TOGGLE(skyPulseGate)
    TECH_VFX_TOGGLE(skyFloatingFrames)
    TECH_VFX_TOGGLE(skyMegaGraph)
    TECH_VFX_TOGGLE(movingPacketRibbon)
    TECH_VFX_TOGGLE(movingSquareTrain)
    TECH_VFX_TOGGLE(risingDataColumn)
    TECH_VFX_TOGGLE(lateralScanPanel)
    TECH_VFX_TOGGLE(driftingGridCloud)
    TECH_VFX_TOGGLE(roamingReticle)
#undef TECH_VFX_TOGGLE

    void handleTick(worldanalysis::sdk::Player* player);
};

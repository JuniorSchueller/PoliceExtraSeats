#include <plugin.h>
#include <CPools.h>
#include <CPed.h>           
#include <CVehicle.h>       
#include <CPlayerPed.h>     
#include <CWanted.h>        
#include <ePedState.h>      
#include <eWeaponType.h>    
#include <extensions/ScriptCommands.h>
#include <extensions/scripting/ScriptCommandNames.h>
#include <CTimer.h>
#include <set>
#include <vector>
#include <algorithm>
#include <map>

using namespace plugin;

constexpr int VEH_POLICE_LA = 596;
constexpr int VEH_POLICE_SF = 597;
constexpr int VEH_POLICE_LV = 598;
constexpr int VEH_POLICE_RURAL = 599;

constexpr int NPC_COP_LA = 280;
constexpr int NPC_COP_SF = 281;
constexpr int NPC_COP_LV = 282;
constexpr int NPC_COP_RURAL = 283;

struct ExtraCopInfo {
    CPed* ped;
    CVehicle* vehicle;
    int designatedSeat;
    bool isReleasedToAI;
};

class PoliceBackSeats
{
    std::set<CVehicle*> processedVehicles;
    std::vector<ExtraCopInfo> activeExtras;

public:
    int GetCopModelForCar(int carModel)
    {
        switch (carModel) {
        case VEH_POLICE_LA:    return NPC_COP_LA;
        case VEH_POLICE_SF:    return NPC_COP_SF;
        case VEH_POLICE_LV:    return NPC_COP_LV;
        case VEH_POLICE_RURAL: return NPC_COP_RURAL;
        default: return -1;
        }
    }

    PoliceBackSeats()
    {
        Events::vehicleDtorEvent += [this](CVehicle* pVeh) {
            processedVehicles.erase(pVeh);
            activeExtras.erase(std::remove_if(activeExtras.begin(), activeExtras.end(),
                [pVeh](const ExtraCopInfo& info) { return info.vehicle == pVeh; }), activeExtras.end());
            };

        Events::pedDtorEvent += [this](CPed* pPed) {
            activeExtras.erase(std::remove_if(activeExtras.begin(), activeExtras.end(),
                [pPed](const ExtraCopInfo& info) { return info.ped == pPed; }), activeExtras.end());
            };

        Events::gameProcessEvent += [this]()
            {
                static int lastCheck = 0;
                if (CTimer::m_snTimeInMilliseconds - lastCheck < 100) return;
                lastCheck = CTimer::m_snTimeInMilliseconds;

                ProcessSpawning();
                ProcessAI();
            };
    }

    void ProcessSpawning()
    {
        for (CVehicle* pVeh : CPools::ms_pVehiclePool)
        {
            if (!pVeh || pVeh->m_fHealth <= 0.0f) continue;

            if (processedVehicles.find(pVeh) != processedVehicles.end()) continue;

            if (pVeh->m_pDriver && pVeh->m_pDriver->IsAlive())
            {
                int pedModel = GetCopModelForCar(pVeh->m_nModelIndex);

                if (pedModel != -1)
                {
                    processedVehicles.insert(pVeh);

                    if (Command<Commands::HAS_MODEL_LOADED>(pedModel))
                    {
                        int hVeh = CPools::GetVehicleRef(pVeh);
                        if (pVeh->m_apPassengers[1] == nullptr) CreateExtraCop(pVeh, hVeh, pedModel, 1);
                        if (pVeh->m_apPassengers[2] == nullptr) CreateExtraCop(pVeh, hVeh, pedModel, 2);
                    }
                }
            }
        }
    }

    void CreateExtraCop(CVehicle* pVeh, int hVeh, int modelID, int seatID)
    {
        int hNewCop;
        Command<Commands::CREATE_CHAR_AS_PASSENGER>(hVeh, PED_TYPE_COP, modelID, seatID, &hNewCop);
        Command<Commands::GIVE_WEAPON_TO_CHAR>(hNewCop, WEAPONTYPE_PISTOL, 9999);

        CPed* pPed = CPools::GetPed(hNewCop);
        if (pPed) {
            activeExtras.push_back({ pPed, pVeh, seatID, false });
        }
    }

    void ProcessAI()
    {
        CPlayerPed* player = FindPlayerPed();
        int playerWantedLevel = 0;

        if (player) {
            CWanted* pWanted = player->GetWanted();
            if (pWanted) playerWantedLevel = pWanted->m_nWantedLevel;
        }

        std::map<CVehicle*, std::vector<ExtraCopInfo*>> squadMap;
        for (auto& info : activeExtras) {
            if (info.ped && info.vehicle && info.vehicle->m_fHealth > 0
                && info.ped->m_ePedState != PEDSTATE_DEAD && info.ped->m_ePedState != PEDSTATE_DIE)
            {
                squadMap[info.vehicle].push_back(&info);
            }
        }

        for (auto& pair : squadMap)
        {
            CVehicle* vehicle = pair.first;
            std::vector<ExtraCopInfo*>& squad = pair.second;
            int hVeh = CPools::GetVehicleRef(vehicle);

            CPed* currentDriver = vehicle->m_pDriver;
            bool isDriverAlive = (currentDriver && currentDriver->IsAlive()
                && currentDriver->m_ePedState != PEDSTATE_DEAD
                && currentDriver->m_ePedState != PEDSTATE_DIE);

            bool isDriverActuallyDriving = (isDriverAlive && currentDriver->m_ePedState == PEDSTATE_DRIVING);

            bool isSirenOn = vehicle->bSirenOrAlarm;

            bool driverIsFighting = false;
            if (isDriverAlive && !isDriverActuallyDriving) {
                if (isSirenOn) driverIsFighting = true;
                if (currentDriver->m_pTargetedObject != nullptr) driverIsFighting = true;
            }

            CVector vehPos = vehicle->GetPosition();
            CVector playPos = player ? player->GetPosition() : CVector(0, 0, 0);
            if (DistanceBetweenPoints(vehPos, playPos) > 300.0f) {
                for (auto* unit : squad) {
                    Command<Commands::MARK_CHAR_AS_NO_LONGER_NEEDED>(CPools::GetPedRef(unit->ped));
                    unit->isReleasedToAI = true;
                }
                continue;
            }

            bool shouldDeploySquad = false;

            if (vehicle->m_fHealth <= 250.0f) shouldDeploySquad = true;
            else if (isSirenOn && (!isDriverActuallyDriving && isDriverAlive)) shouldDeploySquad = true;
            else if (driverIsFighting) shouldDeploySquad = true;
            else if (playerWantedLevel > 1 && !isDriverActuallyDriving) shouldDeploySquad = true;

            if (shouldDeploySquad) {
                for (auto* unit : squad) {
                    if (!unit->isReleasedToAI) {
                        int hPed = CPools::GetPedRef(unit->ped);
                        Command<Commands::CLEAR_CHAR_TASKS>(hPed);
                        Command<Commands::MARK_CHAR_AS_NO_LONGER_NEEDED>(hPed);
                        unit->isReleasedToAI = true;
                    }
                }
                continue;
            }

            bool stragglersOutside = false;

            if (!isDriverAlive) {
                ExtraCopInfo* bestCandidate = nullptr;
                float closestDist = 99999.0f;
                for (auto* unit : squad) {
                    if (unit->designatedSeat == 0) { bestCandidate = unit; break; }
                    float d = (float)DistanceBetweenPoints(unit->ped->GetPosition(), vehPos);
                    if (d < closestDist) { closestDist = d; bestCandidate = unit; }
                }
                if (bestCandidate) bestCandidate->designatedSeat = 0;
            }

            for (auto* unit : squad) {
                bool isInside = (unit->ped->m_pVehicle == vehicle);

                if (isInside) {
                    unit->isReleasedToAI = false;

                    if (unit->ped->m_ePedState == PEDSTATE_EXIT_CAR)
                    {
                        int hPed = CPools::GetPedRef(unit->ped);
                        if (unit->designatedSeat == 0)
                            Command<Commands::TASK_ENTER_CAR_AS_DRIVER>(hPed, hVeh, -1);
                        else
                            Command<Commands::TASK_ENTER_CAR_AS_PASSENGER>(hPed, hVeh, unit->designatedSeat, -1);
                    }
                    else if (unit->designatedSeat != 0)
                    {
                        int hPed = CPools::GetPedRef(unit->ped);
                        if (currentDriver && isDriverAlive) {
                            int hDriver = CPools::GetPedRef(currentDriver);
                            Command<Commands::TASK_LOOK_AT_CHAR>(hPed, hDriver, 300);
                        }
                        else if (player) {
                            int hPlay = CPools::GetPedRef(player);
                            Command<Commands::TASK_LOOK_AT_CHAR>(hPed, hPlay, 300);
                        }
                    }
                    continue;
                }
                else {
                    stragglersOutside = true;
                    if (unit->ped->m_ePedState != PEDSTATE_ENTER_CAR && unit->ped->m_ePedState != PEDSTATE_CARJACK)
                    {
                        int hPed = CPools::GetPedRef(unit->ped);

                        if (unit->designatedSeat == 0)
                            Command<Commands::TASK_ENTER_CAR_AS_DRIVER>(hPed, hVeh, -1);
                        else
                            Command<Commands::TASK_ENTER_CAR_AS_PASSENGER>(hPed, hVeh, unit->designatedSeat, -1);
                    }
                }
            }

            currentDriver = vehicle->m_pDriver;
            if (currentDriver && currentDriver->IsAlive() && stragglersOutside) {
                Command<Commands::SET_CAR_TEMP_ACTION>(hVeh, 2, 200);
            }
        }
    }
} policeBackSeats;

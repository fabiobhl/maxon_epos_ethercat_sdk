/*
** Copyright (2019-2020) Robotics Systems Lab - ETH Zurich:
** Jonas Junger, Johannes Pankert, Fabio Dubois, Lennart Nachtigall,
** Markus Staeuble
**
** This file is part of the maxon_epos_ethercat_sdk.
** The maxon_epos_ethercat_sdk is free software: you can redistribute it and/or
*modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** The maxon_epos_ethercat_sdk is distributed in the hope that it will be
*useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with the maxon_epos_ethercat_sdk. If not, see
*<https://www.gnu.org/licenses/>.
*/

#include <chrono>
#include <cmath>
#include <thread>

#include "maxon_epos_ethercat_sdk/ConfigurationParser.hpp"
#include "maxon_epos_ethercat_sdk/Maxon.hpp"
#include "maxon_epos_ethercat_sdk/ObjectDictionary.hpp"
#include "maxon_epos_ethercat_sdk/RxPdo.hpp"
#include "maxon_epos_ethercat_sdk/TxPdo.hpp"

namespace maxon
{
std::string binstring(uint16_t var)
{
  std::string s = "0000000000000000";
  for (int i = 0; i < 16; i++)
  {
    if (var & (1 << (15 - i)))
    {
      s[i] = '1';
    }
  }
  return s;
}
std::string binstring(int8_t var)
{
  std::string s = "00000000";
  for (int i = 0; i < 8; i++)
  {
    if (var & (1 << (7 - i)))
    {
      s[i] = '1';
    }
  }
  return s;
}

Maxon::SharedPtr Maxon::deviceFromFile(const std::string& configFile, const std::string& name, const uint32_t address)
{
  auto maxon = std::make_shared<Maxon>(name, address);
  maxon->loadConfigFile(configFile);
  return maxon;
}

Maxon::Maxon(const std::string& name, const uint32_t address)
{
  address_ = address;
  name_ = name;
}

bool Maxon::startup()
{
  bool success = true;
  success &= bus_->waitForState(EC_STATE_PRE_OP, address_, 50, 0.05);
  // bus_->syncDistributedClock0(address_, true, timeStep_, timeStep_ / 2.f); //
  // Might not need
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // use hardware motor rated current value if necessary
  // TODO test
  if (configuration_.nominalCurrentA == 0.0)
  {
    uint32_t nominalCurrent;
    success &= sendSdoRead(OD_INDEX_MOTOR_DATA, 0x02, false, nominalCurrent);
    // update the configuration to accomodate the new motor
    // rated current value
    configuration_.nominalCurrentA = static_cast<double>(nominalCurrent) / 1000.0;
    // update the reading_ object to ensure correct unit conversion
    reading_.configureReading(configuration_);
  }
  // success &= setDriveStateViaSdo(DriveState::ReadyToSwitchOn);

  // PDO mapping
  success &= mapPdos(configuration_.rxPdoTypeEnum, configuration_.txPdoTypeEnum);

  // Set Interpolation
  success &= sdoVerifyWrite(OD_INDEX_INTERPOLATION_TIME_PERIOD, 0x01, false, static_cast<uint8_t>(0x01),
                            configuration_.configRunSdoVerifyTimeout);

  std::this_thread::sleep_for(std::chrono::microseconds(configuration_.configRunSdoVerifyTimeout));

  success &= sdoVerifyWrite(OD_INDEX_INTERPOLATION_TIME_PERIOD, 0x02, false, static_cast<int8_t>(-3),
                            configuration_.configRunSdoVerifyTimeout);

  std::this_thread::sleep_for(std::chrono::microseconds(configuration_.configRunSdoVerifyTimeout));

  // Set initial mode of operation
  success &=
      sdoVerifyWrite(OD_INDEX_MODES_OF_OPERATION, 0x00, false, static_cast<int8_t>(configuration_.modeOfOperationEnum),
                     configuration_.configRunSdoVerifyTimeout);
  // To be on the safe side: set currect PDO sizes
  autoConfigurePdoSizes();

  // write the configuration parameters
  success &= configParam(configuration_.modeOfOperationEnum);

  if (!success)
  {
    MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::preStartupOnlineConfiguration] "
                      "hardware configuration of '"
                      << name_ << "' not successful!");
    addErrorToReading(ErrorType::ConfigurationError);
  }
  MELO_INFO_STREAM("Hardware configuration suceeded. success = " << success);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return success;
}

void Maxon::preShutdown()
{
  setDriveStateViaSdo(DriveState::QuickStopActive);
  setDriveStateViaSdo(DriveState::SwitchOnDisabled);
}

void Maxon::shutdown()
{
  bus_->setState(EC_STATE_INIT, address_);
}

void Maxon::updateWrite()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  /*
  ** Check if the Mode of Operation has been set properly
  */
  if (modeOfOperation_ == ModeOfOperationEnum::NA)
  {
    reading_.addError(ErrorType::ModeOfOperationError);
    MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::updateWrite]"
                      " Mode of operation for '"
                      << name_ << "' has not been set.");
    return;
  }

  /*!
   * engage the state machine if a state change is requested
   */
  if (conductStateChange_ && hasRead_)
  {
    engagePdoStateMachine();
  }

  switch (configuration_.rxPdoTypeEnum)
  {
    case RxPdoTypeEnum::RxPdoStandard:
    {
      RxPdoStandard rxPdo{};
      rxPdo.modeOfOperation_ = static_cast<int8_t>(modeOfOperation_);
      rxPdo.controlWord_ = controlword_.getRawControlword();

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    case RxPdoTypeEnum::RxPdoCSP:
    {
      RxPdoCSP rxPdo{};
      rxPdo.targetPosition_ = stagedCommand_.getTargetPositionRaw();
      rxPdo.positionOffset_ = stagedCommand_.getPositionOffsetRaw();
      rxPdo.torqueOffset_ = stagedCommand_.getTorqueOffsetRaw();

      // Extra data
      rxPdo.controlWord_ = controlword_.getRawControlword();
      rxPdo.modeOfOperation_ = static_cast<int8_t>(stagedCommand_.getModeOfOperation());

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    case RxPdoTypeEnum::RxPdoCST:
    {
      RxPdoCST rxPdo{};
      rxPdo.targetTorque_ = stagedCommand_.getTargetTorqueRaw();
      rxPdo.torqueOffset_ = stagedCommand_.getTorqueOffsetRaw();

      // Extra data
      rxPdo.controlWord_ = controlword_.getRawControlword();
      rxPdo.modeOfOperation_ = static_cast<int8_t>(stagedCommand_.getModeOfOperation());

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    case RxPdoTypeEnum::RxPdoCSV:
    {
      RxPdoCSV rxPdo{};
      rxPdo.targetVelocity_ = stagedCommand_.getTargetVelocityRaw();
      rxPdo.velocityOffset_ = stagedCommand_.getVelocityOffsetRaw();

      // Extra data
      rxPdo.controlWord_ = controlword_.getRawControlword();
      rxPdo.modeOfOperation_ = static_cast<int8_t>(stagedCommand_.getModeOfOperation());

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    case RxPdoTypeEnum::RxPdoCSTCSP:
    {
      RxPdoCSTCSP rxPdo{};

      // Extra data
      rxPdo.controlWord_ = controlword_.getRawControlword();
      rxPdo.modeOfOperation_ = static_cast<int8_t>(stagedCommand_.getModeOfOperation());

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    case RxPdoTypeEnum::RxPdoPVM:
    {
      RxPdoPVM rxPdo{};
      rxPdo.controlWord_ = controlword_.getRawControlword();
      rxPdo.targetVelocity_ = stagedCommand_.getTargetVelocityRaw();
      rxPdo.profileAccel_ = stagedCommand_.getProfileAccelRaw();
      rxPdo.profileDeccel_ = stagedCommand_.getProfileDeccelRaw();
      rxPdo.motionProfileType_ = stagedCommand_.getMotionProfileType();

      // actually writing to the hardware
      bus_->writeRxPdo(address_, rxPdo);
      break;
    }
    default:
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::updateWrite] "
                        " Unsupported Rx Pdo type for '"
                        << name_ << "'");
      addErrorToReading(ErrorType::RxPdoTypeError);
  }
}

void Maxon::updateRead()
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  // TODO(duboisf): implement some sort of time stamp
  switch (configuration_.txPdoTypeEnum)
  {
    case TxPdoTypeEnum::TxPdoStandard:
    {
      TxPdoStandard txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);
      reading_.setStatusword(txPdo.statusword_);
      break;
    }
    case TxPdoTypeEnum::TxPdoCSP:
    {
      TxPdoCSP txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);
      reading_.setStatusword(txPdo.statusword_);
      reading_.setActualCurrent(txPdo.actualTorque_);
      reading_.setActualVelocity(txPdo.actualVelocity_);
      reading_.setActualPosition(txPdo.actualPosition_);
      break;
    }
    case TxPdoTypeEnum::TxPdoCST:
    {
      TxPdoCST txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);
      reading_.setStatusword(txPdo.statusword_);
      reading_.setActualCurrent(txPdo.actualTorque_);
      reading_.setActualVelocity(txPdo.actualVelocity_);
      reading_.setActualPosition(txPdo.actualPosition_);
      break;
    }
    case TxPdoTypeEnum::TxPdoCSV:
    {
      TxPdoCSV txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);
      reading_.setStatusword(txPdo.statusword_);
      reading_.setActualCurrent(txPdo.actualTorque_);
      reading_.setActualVelocity(txPdo.actualVelocity_);
      reading_.setActualPosition(txPdo.actualPosition_);
      break;
    }
    case TxPdoTypeEnum::TxPdoCSTCSP:
    {
      TxPdoCSTCSP txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);
      reading_.setStatusword(txPdo.statusword_);
      reading_.setActualCurrent(txPdo.actualTorque_);
      reading_.setActualVelocity(txPdo.actualVelocity_);
      reading_.setActualPosition(txPdo.actualPosition_);
      break;
    }
    case TxPdoTypeEnum::TxPdoPVM:
    {
      TxPdoPVM txPdo{};
      // reading from the bus
      bus_->readTxPdo(address_, txPdo);

      reading_.setDemandVelocity(txPdo.demandVelocity_);
      reading_.setStatusword(txPdo.statusword_);
      break;
    }
    default:
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::updateRrite] Unsupported Tx Pdo "
                        "type for '"
                        << name_ << "'");
      reading_.addError(ErrorType::TxPdoTypeError);
  }

  // set the hasRead_ variable to true since a nes reading was read
  if (!hasRead_)
  {
    hasRead_ = true;
  }

  // Print warning if drive is in Fault state.
  if (reading_.getDriveState() == DriveState::Fault)
  {
    MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::updateRead] '" << name_ << "' is in drive state 'Fault'");
    printErrorCode();
  }
}

void Maxon::stageCommand(const Command& command)
{
  std::lock_guard<std::recursive_mutex> lock(stagedCommandMutex_);
  stagedCommand_ = command;
  stagedCommand_.setPositionFactorRadToInteger(static_cast<double>(configuration_.positionEncoderResolution) /
                                               (2.0 * M_PI));

  double currentFactorAToInt = 1000.0 / configuration_.nominalCurrentA;
  stagedCommand_.setCurrentFactorAToInteger(currentFactorAToInt);
  stagedCommand_.setTorqueFactorNmToInteger(currentFactorAToInt / configuration_.motorConstant /
                                            configuration_.gearRatio);

  stagedCommand_.setMaxCurrent(configuration_.maxCurrentA);
  stagedCommand_.setMaxTorque(configuration_.maxCurrentA * configuration_.motorConstant * configuration_.gearRatio);

  stagedCommand_.setUseRawCommands(configuration_.useRawCommands);

  stagedCommand_.doUnitConversion();

  if (allowModeChange_)
  {
    modeOfOperation_ = command.getModeOfOperation();
  }
  else
  {
    if (modeOfOperation_ != command.getModeOfOperation() && command.getModeOfOperation() != ModeOfOperationEnum::NA)
    {
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::stageCommand] Changing the mode of "
                        "operation of '"
                        << name_ << "' is not allowed for the active configuration.");
    }
  }
}

Reading Maxon::getReading() const
{
  std::lock_guard<std::recursive_mutex> lock(readingMutex_);
  return reading_;
}

void Maxon::getReading(Reading& reading) const
{
  std::lock_guard<std::recursive_mutex> lock(readingMutex_);
  reading = reading_;
}

bool Maxon::loadConfigFile(const std::string& fileName)
{
  ConfigurationParser configurationParser(fileName);
  return loadConfiguration(configurationParser.getConfiguration());
}

bool Maxon::loadConfigNode(YAML::Node configNode)
{
  ConfigurationParser configurationParser(configNode);
  return loadConfiguration(configurationParser.getConfiguration());
}

bool Maxon::loadConfiguration(const Configuration& configuration)
{
  bool success = true;
  reading_.configureReading(configuration);

  // Check if changing mode of operation will be allowed
  allowModeChange_ = true;
  allowModeChange_ &= configuration.useMultipleModeOfOperations;
  allowModeChange_ &= (configuration.rxPdoTypeEnum == RxPdoTypeEnum::RxPdoStandard);
  allowModeChange_ &= (configuration.txPdoTypeEnum == TxPdoTypeEnum::TxPdoStandard);

  modeOfOperation_ = configuration.modeOfOperationEnum;

  configuration_ = configuration;
  return success;
}

Configuration Maxon::getConfiguration() const
{
  return configuration_;
}

bool Maxon::getStatuswordViaSdo(Statusword& statusword)
{
  uint16_t statuswordValue = 0;
  bool success = sendSdoRead(OD_INDEX_STATUSWORD, 0, false, statuswordValue);
  statusword.setFromRawStatusword(statuswordValue);
  return success;
}

bool Maxon::setControlwordViaSdo(Controlword& controlword)
{
  return sendSdoWrite(OD_INDEX_CONTROLWORD, 0, false, controlword.getRawControlword());
}

bool Maxon::setDriveStateViaSdo(const DriveState& driveState)
{
  bool success = true;
  Statusword currentStatusword;
  success &= getStatuswordViaSdo(currentStatusword);
  DriveState currentDriveState = currentStatusword.getDriveState();

  // do the adequate state changes (via sdo) depending on the requested and
  // current drive states
  switch (driveState)
  {
    // Target: switch on disabled
    // This is the lowest state in which the state machine can be brought over
    // EtherCAT
    case DriveState::SwitchOnDisabled:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          success &= true;
          break;
        case DriveState::ReadyToSwitchOn:
          success &= stateTransitionViaSdo(StateTransition::_7);
          break;
        case DriveState::SwitchedOn:
          success &= stateTransitionViaSdo(StateTransition::_10);
          break;
        case DriveState::OperationEnabled:
          success &= stateTransitionViaSdo(StateTransition::_9);
          break;
        case DriveState::QuickStopActive:
          success &= stateTransitionViaSdo(StateTransition::_12);
          break;
        case DriveState::Fault:
          success &= stateTransitionViaSdo(StateTransition::_15);
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                            "Transition not implemented");
          addErrorToReading(ErrorType::SdoStateTransitionError);
          success = false;
      }
      break;

    case DriveState::ReadyToSwitchOn:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          success &= stateTransitionViaSdo(StateTransition::_2);
          break;
        case DriveState::ReadyToSwitchOn:
          success &= true;
          break;
        case DriveState::SwitchedOn:
          success &= stateTransitionViaSdo(StateTransition::_6);
          break;
        case DriveState::OperationEnabled:
          success &= stateTransitionViaSdo(StateTransition::_8);
          break;
        case DriveState::QuickStopActive:
          success &= stateTransitionViaSdo(StateTransition::_12);
          success &= stateTransitionViaSdo(StateTransition::_2);
          break;
        case DriveState::Fault:
          success &= stateTransitionViaSdo(StateTransition::_15);
          success &= stateTransitionViaSdo(StateTransition::_2);
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                            "Transition not implemented");
          addErrorToReading(ErrorType::SdoStateTransitionError);
          success = false;
      }
      break;

    case DriveState::SwitchedOn:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          break;
        case DriveState::ReadyToSwitchOn:
          success &= stateTransitionViaSdo(StateTransition::_3);
          break;
        case DriveState::SwitchedOn:
          success &= true;
          break;
        case DriveState::OperationEnabled:
          success &= stateTransitionViaSdo(StateTransition::_5);
          break;
        case DriveState::QuickStopActive:
          success &= stateTransitionViaSdo(StateTransition::_12);
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          break;
        case DriveState::Fault:
          success &= stateTransitionViaSdo(StateTransition::_15);
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                            "Transition not implemented");
          addErrorToReading(ErrorType::SdoStateTransitionError);
          success = false;
      }
      break;

    case DriveState::OperationEnabled:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          break;
        case DriveState::ReadyToSwitchOn:
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          break;
        case DriveState::SwitchedOn:
          success &= stateTransitionViaSdo(StateTransition::_4);
          break;
        case DriveState::OperationEnabled:
          success &= true;
          break;
        case DriveState::QuickStopActive:
          success &= stateTransitionViaSdo(StateTransition::_12);
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          break;
        case DriveState::Fault:
          success &= stateTransitionViaSdo(StateTransition::_15);
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                            "Transition not implemented");
          addErrorToReading(ErrorType::SdoStateTransitionError);
          success = false;
      }
      break;

    case DriveState::QuickStopActive:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          success &= stateTransitionViaSdo(StateTransition::_11);
          break;
        case DriveState::ReadyToSwitchOn:
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          success &= stateTransitionViaSdo(StateTransition::_11);
          break;
        case DriveState::SwitchedOn:
          success &= stateTransitionViaSdo(StateTransition::_4);
          success &= stateTransitionViaSdo(StateTransition::_11);
          break;
        case DriveState::OperationEnabled:
          success &= stateTransitionViaSdo(StateTransition::_11);
          break;
        case DriveState::QuickStopActive:
          success &= true;
          break;
        case DriveState::Fault:
          success &= stateTransitionViaSdo(StateTransition::_15);
          success &= stateTransitionViaSdo(StateTransition::_2);
          success &= stateTransitionViaSdo(StateTransition::_3);
          success &= stateTransitionViaSdo(StateTransition::_4);
          success &= stateTransitionViaSdo(StateTransition::_11);
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                            "Transition not implemented");
          addErrorToReading(ErrorType::SdoStateTransitionError);
          success = false;
      }
      break;

    default:
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::setDriveStateViaSdo] State "
                        "Transition not implemented");
      addErrorToReading(ErrorType::SdoStateTransitionError);
      success = false;
  }
  return success;
}

bool Maxon::stateTransitionViaSdo(const StateTransition& stateTransition)
{
  Controlword controlword;
  switch (stateTransition)
  {
    case StateTransition::_2:
      controlword.setStateTransition2();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_3:
      controlword.setStateTransition3();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_4:
      controlword.setStateTransition4();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_5:
      controlword.setStateTransition5();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_6:
      controlword.setStateTransition6();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_7:
      controlword.setStateTransition7();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_8:
      controlword.setStateTransition8();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_9:
      controlword.setStateTransition9();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_10:
      controlword.setStateTransition10();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_11:
      controlword.setStateTransition11();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_12:
      controlword.setStateTransition12();
      return setControlwordViaSdo(controlword);
      break;
    case StateTransition::_15:
      controlword.setStateTransition15();
      return setControlwordViaSdo(controlword);
      break;
    default:
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::stateTransitionViaSdo] State "
                        "Transition not implemented");
      addErrorToReading(ErrorType::SdoStateTransitionError);
      return false;
  }
}

bool Maxon::setDriveStateViaPdo(const DriveState& driveState, const bool waitForState)
{
  bool success = false;
  /*
  ** locking the mutex_
  ** This is not done with a lock_guard here because during the waiting time the
  ** mutex_ must be unlocked periodically such that PDO writing (and thus state
  ** changes) may occur at all!
  */
  mutex_.lock();

  // reset the "stateChangeSuccessful_" flag to false such that a new successful
  // state change can be detected
  stateChangeSuccessful_ = false;

  // make the state machine realize that a state change will have to happen
  conductStateChange_ = true;

  // overwrite the target drive state
  targetDriveState_ = driveState;

  // set the hasRead flag to false such that at least one new reading will be
  // available when starting the state change
  hasRead_ = false;

  // set the time point of the last pdo change to now
  driveStateChangeTimePoint_ = std::chrono::steady_clock::now();

  // set a temporary time point to prevent getting caught in an infinite loop
  auto driveStateChangeStartTimePoint = std::chrono::steady_clock::now();

  // return if no waiting is requested
  if (!waitForState)
  {
    // unlock the mutex
    mutex_.unlock();
    // return true if no waiting is requested
    return true;
  }

  // Wait for the state change to be successful
  // during the waiting time the mutex MUST be unlocked!

  while (true)
  {
    // break loop as soon as the state change was successful
    if (stateChangeSuccessful_)
    {
      success = true;
      break;
    }

    // break the loop if the state change takes too long
    // this prevents a freezing of the end user's program if the hardware is not
    // able to change it's state.
    if ((std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               driveStateChangeStartTimePoint))
            .count() > configuration_.driveStateChangeMaxTimeout)
    {
      break;
    }
    // unlock the mutex during sleep time
    mutex_.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // lock the mutex to be able to check the success flag
    mutex_.lock();
  }
  // unlock the mutex one last time
  mutex_.unlock();
  return success;
}

Controlword Maxon::getNextStateTransitionControlword(const DriveState& requestedDriveState,
                                                     const DriveState& currentDriveState)
{
  Controlword controlword;
  controlword.setAllFalse();
  switch (requestedDriveState)
  {
    case DriveState::SwitchOnDisabled:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "drive state has already been reached for '" << name_ << "'");
          addErrorToReading(ErrorType::PdoStateTransitionError);
          break;
        case DriveState::ReadyToSwitchOn:
          controlword.setStateTransition7();
          break;
        case DriveState::SwitchedOn:
          controlword.setStateTransition10();
          break;
        case DriveState::OperationEnabled:
          controlword.setStateTransition9();
          break;
        case DriveState::QuickStopActive:
          controlword.setStateTransition12();
          break;
        case DriveState::Fault:
          controlword.setStateTransition15();
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "PDO state transition not implemented for '" << name_ << "'\n"
                            << "Current: " << currentDriveState << "\n"
                            << "Requested: " << requestedDriveState);
          addErrorToReading(ErrorType::PdoStateTransitionError);
      }
      break;

    case DriveState::ReadyToSwitchOn:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          controlword.setStateTransition2();
          break;
        case DriveState::ReadyToSwitchOn:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "drive state has already been reached for '" << name_ << "'");
          addErrorToReading(ErrorType::PdoStateTransitionError);
          break;
        case DriveState::SwitchedOn:
          controlword.setStateTransition6();
          break;
        case DriveState::OperationEnabled:
          controlword.setStateTransition8();
          break;
        case DriveState::QuickStopActive:
          controlword.setStateTransition12();
          break;
        case DriveState::Fault:
          controlword.setStateTransition15();
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "PDO state transition not implemented for '" << name_ << "'\n"
                            << "Current: " << currentDriveState << "\n"
                            << "Requested: " << requestedDriveState);
          addErrorToReading(ErrorType::PdoStateTransitionError);
      }
      break;

    case DriveState::SwitchedOn:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          controlword.setStateTransition2();
          break;
        case DriveState::ReadyToSwitchOn:
          controlword.setStateTransition3();
          break;
        case DriveState::SwitchedOn:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "drive state has already been reached for '" << name_ << "'");
          addErrorToReading(ErrorType::PdoStateTransitionError);
          break;
        case DriveState::OperationEnabled:
          controlword.setStateTransition5();
          break;
        case DriveState::QuickStopActive:
          controlword.setStateTransition12();
          break;
        case DriveState::Fault:
          controlword.setStateTransition15();
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "PDO state transition not implemented for '" << name_ << "'\n"
                            << "Current: " << currentDriveState << "\n"
                            << "Requested: " << requestedDriveState);
          addErrorToReading(ErrorType::PdoStateTransitionError);
      }
      break;

    case DriveState::OperationEnabled:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          controlword.setStateTransition2();
          break;
        case DriveState::ReadyToSwitchOn:
          controlword.setStateTransition3();
          break;
        case DriveState::SwitchedOn:
          controlword.setStateTransition4();
          break;
        case DriveState::OperationEnabled:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "drive state has already been reached for '" << name_ << "'");
          addErrorToReading(ErrorType::PdoStateTransitionError);
          break;
        case DriveState::QuickStopActive:
          controlword.setStateTransition12();
          break;
        case DriveState::Fault:
          controlword.setStateTransition15();
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "PDO state transition not implemented for '" << name_ << "'\n"
                            << "Current: " << currentDriveState << "\n"
                            << "Requested: " << requestedDriveState);
          addErrorToReading(ErrorType::PdoStateTransitionError);
      }
      break;

    case DriveState::QuickStopActive:
      switch (currentDriveState)
      {
        case DriveState::SwitchOnDisabled:
          controlword.setStateTransition2();
          break;
        case DriveState::ReadyToSwitchOn:
          controlword.setStateTransition3();
          break;
        case DriveState::SwitchedOn:
          controlword.setStateTransition4();
          break;
        case DriveState::OperationEnabled:
          controlword.setStateTransition11();
          break;
        case DriveState::QuickStopActive:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "drive state has already been reached for '" << name_ << "'");
          addErrorToReading(ErrorType::PdoStateTransitionError);
          break;
        case DriveState::Fault:
          controlword.setStateTransition15();
          break;
        default:
          MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::"
                            "getNextStateTransitionControlword] "
                            << "PDO state transition not implemented for '" << name_ << "'\n"
                            << "Current: " << currentDriveState << "\n"
                            << "Requested: " << requestedDriveState);
          addErrorToReading(ErrorType::PdoStateTransitionError);
      }
      break;

    default:
      MELO_ERROR_STREAM("[maxon_epos_ethercat_sdk:Maxon::getNextStateTransitionControlword] "
                        << "PDO state cannot be reached for '" << name_ << "'");
      addErrorToReading(ErrorType::PdoStateTransitionError);
  }

  return controlword;
}

void Maxon::autoConfigurePdoSizes()
{
  auto pdoSizes = bus_->getHardwarePdoSizes(static_cast<uint16_t>(address_));
  pdoInfo_.rxPdoSize_ = pdoSizes.first;
  pdoInfo_.txPdoSize_ = pdoSizes.second;
}

uint16_t Maxon::getTxPdoSize()
{
  return pdoInfo_.txPdoSize_;
}

uint16_t Maxon::getRxPdoSize()
{
  return pdoInfo_.rxPdoSize_;
}

void Maxon::engagePdoStateMachine()
{
  std::cout << "Pdo State Machine engaged" << std::endl;
  std::cout << "Conduct state change: " << conductStateChange_ << std::endl;
  // locking the mutex
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  // elapsed time since the last new controlword
  auto microsecondsSinceChange = (std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - driveStateChangeTimePoint_))
                                     .count();

  // get the current state
  // since we wait until "hasRead" is true, this is guaranteed to be a newly
  // read value
  const DriveState currentDriveState = reading_.getDriveState();
  std::cout << "Current drive state is: " << currentDriveState << std::endl;

  // check if the state change already was successful:
  if (currentDriveState == targetDriveState_)
  {
    std::cout << "Correct target state reached" << std::endl;
    numberOfSuccessfulTargetStateReadings_++;
    if (numberOfSuccessfulTargetStateReadings_ >= configuration_.minNumberOfSuccessfulTargetStateReadings)
    {
      // disable the state machine
      conductStateChange_ = false;
      numberOfSuccessfulTargetStateReadings_ = 0;
      stateChangeSuccessful_ = true;
      return;
    }
  }
  else if (microsecondsSinceChange > configuration_.driveStateChangeMinTimeout)
  {
    // get the next controlword from the state machine
    controlword_ = getNextStateTransitionControlword(targetDriveState_, currentDriveState);
    driveStateChangeTimePoint_ = std::chrono::steady_clock::now();
  }

  // set the "hasRead" variable to false such that there will definitely be a
  // new reading when this method is called again
  hasRead_ = false;
}
}  // namespace maxon

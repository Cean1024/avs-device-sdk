/*
 * Copyright 2017-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "AFML/FocusManager.h"
#include <AVSCommon/Utils/Logger/Logger.h>

namespace alexaClientSDK {
namespace afml {

using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::utils;
using namespace avsCommon::avs;

/// String to identify log entries originating from this file.
static const std::string TAG("FocusManager");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

FocusManager::FocusManager(
    const std::vector<ChannelConfiguration> channelConfigurations,
    std::shared_ptr<ActivityTrackerInterface> activityTrackerInterface) :
        m_activityTracker{activityTrackerInterface} {
    for (auto config : channelConfigurations) {
        if (doesChannelNameExist(config.name)) {
            ACSDK_ERROR(LX("createChannelFailed").d("reason", "channelNameExists").d("config", config.toString()));
            continue;
        }
        if (doesChannelPriorityExist(config.priority)) {
            ACSDK_ERROR(LX("createChannelFailed").d("reason", "channelPriorityExists").d("config", config.toString()));
            continue;
        }

        auto channel = std::make_shared<Channel>(config.name, config.priority);
        m_allChannels.insert({config.name, channel});
    }
}

bool FocusManager::acquireChannel(
    const std::string& channelName,
    std::shared_ptr<ChannelObserverInterface> channelObserver,
    const std::string& interface) {
    ACSDK_DEBUG1(LX("acquireChannel").d("channelName", channelName).d("interface", interface));
    std::shared_ptr<Channel> channelToAcquire = getChannel(channelName);
    if (!channelToAcquire) {
        ACSDK_ERROR(LX("acquireChannelFailed").d("reason", "channelNotFound").d("channelName", channelName));
        return false;
    }

    m_executor.submit([this, channelToAcquire, channelObserver, interface]() {
        acquireChannelHelper(channelToAcquire, channelObserver, interface);
    });
    return true;
}

std::future<bool> FocusManager::releaseChannel(
    const std::string& channelName,
    std::shared_ptr<ChannelObserverInterface> channelObserver) {
    ACSDK_DEBUG1(LX("releaseChannel").d("channelName", channelName));

    // Using a shared_ptr here so that the promise stays in scope by the time the Executor picks up the task.
    auto releaseChannelSuccess = std::make_shared<std::promise<bool>>();
    std::future<bool> returnValue = releaseChannelSuccess->get_future();
    std::shared_ptr<Channel> channelToRelease = getChannel(channelName);
    if (!channelToRelease) {
        ACSDK_ERROR(LX("releaseChannelFailed").d("reason", "channelNotFound").d("channelName", channelName));
        releaseChannelSuccess->set_value(false);
        return returnValue;
    }

    m_executor.submit([this, channelToRelease, channelObserver, releaseChannelSuccess, channelName]() {
        releaseChannelHelper(channelToRelease, channelObserver, releaseChannelSuccess, channelName);
    });

    return returnValue;
}

void FocusManager::stopForegroundActivity() {
    // We lock these variables so that we can correctly capture the currently foregrounded channel and activity.
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> foregroundChannel = getHighestPriorityActiveChannelLocked();
    if (!foregroundChannel) {
        ACSDK_DEBUG(LX("stopForegroundActivityFailed").d("reason", "noForegroundActivity"));
        return;
    }

    std::string foregroundChannelInterface = foregroundChannel->getInterface();
    lock.unlock();

    m_executor.submitToFront([this, foregroundChannel, foregroundChannelInterface]() {
        stopForegroundActivityHelper(foregroundChannel, foregroundChannelInterface);
    });
}

void FocusManager::stopAllActivities() {
    ACSDK_DEBUG5(LX(__func__));

    if (m_activeChannels.empty()) {
        ACSDK_DEBUG5(LX(__func__).m("no active channels"));
        return;
    }

    ChannelsToInterfaceNamesMap channelOwnersCapture;
    std::unique_lock<std::mutex> lock(m_mutex);

    for (const auto& channel : m_activeChannels) {
        channelOwnersCapture.insert(std::pair<std::shared_ptr<Channel>, std::string>(channel, channel->getInterface()));
    }

    lock.unlock();

    m_executor.submitToFront([this, channelOwnersCapture]() { stopAllActivitiesHelper(channelOwnersCapture); });
}

void FocusManager::addObserver(const std::shared_ptr<FocusManagerObserverInterface>& observer) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_observers.insert(observer);
}

void FocusManager::removeObserver(const std::shared_ptr<FocusManagerObserverInterface>& observer) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_observers.erase(observer);
}

void FocusManager::setChannelFocus(const std::shared_ptr<Channel>& channel, FocusState focus) {
    if (!channel->setFocus(focus)) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    auto observers = m_observers;
    lock.unlock();
    for (auto& observer : observers) {
        observer->onFocusChanged(channel->getName(), focus);
    }
    m_activityUpdates.push_back(channel->getState());
}

void FocusManager::acquireChannelHelper(
    std::shared_ptr<Channel> channelToAcquire,
    std::shared_ptr<ChannelObserverInterface> channelObserver,
    const std::string& interface) {
    // Notify the old observer, if there is one, that it lost focus.
    setChannelFocus(channelToAcquire, FocusState::NONE);

    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> foregroundChannel = getHighestPriorityActiveChannelLocked();
    channelToAcquire->setInterface(interface);
    m_activeChannels.insert(channelToAcquire);
    lock.unlock();

    // Set the new observer.
    channelToAcquire->setObserver(channelObserver);

    if (!foregroundChannel) {
        setChannelFocus(channelToAcquire, FocusState::FOREGROUND);
    } else if (foregroundChannel == channelToAcquire) {
        setChannelFocus(channelToAcquire, FocusState::FOREGROUND);
    } else if (*channelToAcquire > *foregroundChannel) {
        setChannelFocus(foregroundChannel, FocusState::BACKGROUND);
        setChannelFocus(channelToAcquire, FocusState::FOREGROUND);
    } else {
        setChannelFocus(channelToAcquire, FocusState::BACKGROUND);
    }
    notifyActivityTracker();
}

void FocusManager::releaseChannelHelper(
    std::shared_ptr<Channel> channelToRelease,
    std::shared_ptr<ChannelObserverInterface> channelObserver,
    std::shared_ptr<std::promise<bool>> releaseChannelSuccess,
    const std::string& name) {
    if (!channelToRelease->doesObserverOwnChannel(channelObserver)) {
        ACSDK_ERROR(LX("releaseChannelHelperFailed").d("reason", "observerDoesNotOwnChannel").d("channel", name));
        releaseChannelSuccess->set_value(false);
        return;
    }

    releaseChannelSuccess->set_value(true);
    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    bool wasForegrounded = isChannelForegroundedLocked(channelToRelease);
    m_activeChannels.erase(channelToRelease);
    lock.unlock();

    setChannelFocus(channelToRelease, FocusState::NONE);
    if (wasForegrounded) {
        foregroundHighestPriorityActiveChannel();
    }
    notifyActivityTracker();
}

void FocusManager::stopForegroundActivityHelper(
    std::shared_ptr<Channel> foregroundChannel,
    std::string foregroundChannelInterface) {
    if (foregroundChannelInterface != foregroundChannel->getInterface()) {
        return;
    }
    if (!foregroundChannel->hasObserver()) {
        return;
    }
    setChannelFocus(foregroundChannel, FocusState::NONE);

    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_activeChannels.erase(foregroundChannel);
    lock.unlock();
    foregroundHighestPriorityActiveChannel();
    notifyActivityTracker();
}

void FocusManager::stopAllActivitiesHelper(const ChannelsToInterfaceNamesMap& channelsOwnersMap) {
    ACSDK_DEBUG3(LX(__func__));

    std::set<std::shared_ptr<Channel>> channelsToClear;

    std::unique_lock<std::mutex> lock(m_mutex);

    for (const auto& channelAndInterface : channelsOwnersMap) {
        if (channelAndInterface.first->getInterface() == channelAndInterface.second) {
            m_activeChannels.erase(channelAndInterface.first);
            channelsToClear.insert(channelAndInterface.first);
        } else {
            ACSDK_INFO(LX(__func__)
                           .d("reason", "channel has other ownership")
                           .d("channel", channelAndInterface.first->getName())
                           .d("currentInterface", channelAndInterface.first->getInterface())
                           .d("originalInterface", channelAndInterface.second));
        }
    }

    lock.unlock();

    for (const auto& channel : channelsToClear) {
        setChannelFocus(channel, FocusState::NONE);
    }
    foregroundHighestPriorityActiveChannel();
    notifyActivityTracker();
}

std::shared_ptr<Channel> FocusManager::getChannel(const std::string& channelName) const {
    auto search = m_allChannels.find(channelName);
    if (search != m_allChannels.end()) {
        return search->second;
    }
    return nullptr;
}

std::shared_ptr<Channel> FocusManager::getHighestPriorityActiveChannelLocked() const {
    if (m_activeChannels.empty()) {
        return nullptr;
    }
    return *m_activeChannels.begin();
}

bool FocusManager::isChannelForegroundedLocked(const std::shared_ptr<Channel>& channel) const {
    return getHighestPriorityActiveChannelLocked() == channel;
}

bool FocusManager::doesChannelNameExist(const std::string& name) const {
    return m_allChannels.find(name) != m_allChannels.end();
}

bool FocusManager::doesChannelPriorityExist(const unsigned int priority) const {
    for (auto it = m_allChannels.begin(); it != m_allChannels.end(); ++it) {
        if (it->second->getPriority() == priority) {
            return true;
        }
    }
    return false;
}

void FocusManager::foregroundHighestPriorityActiveChannel() {
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> channelToForeground = getHighestPriorityActiveChannelLocked();
    lock.unlock();

    if (channelToForeground) {
        setChannelFocus(channelToForeground, FocusState::FOREGROUND);
    }
}

void FocusManager::notifyActivityTracker() {
    if (m_activityTracker) {
        m_activityTracker->notifyOfActivityUpdates(m_activityUpdates);
    }
    m_activityUpdates.clear();
}

const std::vector<FocusManager::ChannelConfiguration> FocusManager::getDefaultAudioChannels() {
    static const std::vector<FocusManager::ChannelConfiguration> defaultAudioChannels = {
        {FocusManagerInterface::DIALOG_CHANNEL_NAME, FocusManagerInterface::DIALOG_CHANNEL_PRIORITY},
        {FocusManagerInterface::ALERT_CHANNEL_NAME, FocusManagerInterface::ALERT_CHANNEL_PRIORITY},
        {FocusManagerInterface::COMMUNICATIONS_CHANNEL_NAME, FocusManagerInterface::COMMUNICATIONS_CHANNEL_PRIORITY},
        {FocusManagerInterface::CONTENT_CHANNEL_NAME, FocusManagerInterface::CONTENT_CHANNEL_PRIORITY}};

    return defaultAudioChannels;
}

const std::vector<FocusManager::ChannelConfiguration> FocusManager::getDefaultVisualChannels() {
    static const std::vector<FocusManager::ChannelConfiguration> defaultVisualChannels = {
        {FocusManagerInterface::VISUAL_CHANNEL_NAME, FocusManagerInterface::VISUAL_CHANNEL_PRIORITY}};

    return defaultVisualChannels;
}

}  // namespace afml
}  // namespace alexaClientSDK

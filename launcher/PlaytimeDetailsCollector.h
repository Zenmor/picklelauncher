// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - reads the instances behind one row of the Playtime dialog (pickle-playtime)
 *
 *  Only reads: nothing is loaded into an instance, saved or resolved. Minecraft and loader versions come from the
 *  component list of the instance when it is loaded already, else straight from its mmc-pack.json.
 */

#pragma once

#include <QString>

#include "PlaytimeDetails.h"

class InstanceList;

namespace PlaytimeDetails {

/**
 * Details of the row with the given key (PlaytimeBreakdown::Row::key). The instances are the same ones that
 * InstanceList::playtimeEntries() counts: hidden instances are included, instances whose folder is gone are not.
 * A key that matches nothing gives details without instances.
 */
Details collect(InstanceList& instances, const QString& rowKey, const QString& rowName);

}  // namespace PlaytimeDetails

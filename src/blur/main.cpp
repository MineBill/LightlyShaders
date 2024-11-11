/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"

namespace Lightly
{

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(Lightly::BlurEffect,
                                      "metadata.json",
                                      return Lightly::BlurEffect::supported();
                                      ,
                                      return Lightly::BlurEffect::enabledByDefault();)

} // namespace Lightly

#include "main.moc"

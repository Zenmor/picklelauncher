// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include "SharedPackManifest.h"

namespace SharedPack {

/// Where a pack lives and how to read it: the thing the owner sends to the members as `picklepack:1:<base64url(JSON)>`.
/// It holds secrets (the token and, in S3 mode, read keys): it is never logged, see Invite::secrets and Redactor.
struct Invite {
    enum class Mode {
        Public,  ///< any HTTPS host that serves static files: the unguessable token in the path is the only secret
        S3,      ///< an S3-compatible bucket read with SigV4 and read-only keys (Cloudflare R2 etc.), path-style
    };

    int version = 1;          ///< 1 (S3 or legacy public), 2 (multi-endpoint public, no keys)
    QString packId;
    QString name;             ///< optional, only for display before the first download
    QString endpoint;         ///< primary https origin
    QStringList endpoints;    ///< v2: read base URLs (primary first, fallbacks after)
    QString bucket;           ///< S3 only
    QString prefix;           ///< optional folder inside the bucket / below the endpoint, "a/b", no slashes at the ends
    QString region;           ///< S3 only, "auto" on R2
    QString token;            ///< unguessable folder of the pack; rotating the link replaces it
    QString accessKeyId;      ///< S3 only
    QString secretAccessKey;  ///< S3 only

    Mode mode() const { return accessKeyId.isEmpty() ? Mode::Public : Mode::S3; }

    /// The strings to hand to the Redactor.
    QStringList secrets() const;

    /// The absolute URL of \a relative ("latest.json", "revisions/3.json", "files/<sha512>") for this pack.
    QUrl urlFor(const QString& relative, int endpointIndex = 0) const;
    /// The same, as the percent-decoded path the request is signed over (S3 mode), "/bucket/prefix/token/relative".
    QString signedPathFor(const QString& relative, int endpointIndex = 0) const;
    /// The host (and port) of \a endpointIndex.
    QString host(int endpointIndex = 0) const;
    /// All authorities this invite may connect to.
    QStringList allHosts() const;
};

constexpr int MaxInviteFormatVersion = 2;
constexpr int MaxInviteChars = 8 * 1024;

/// Parses and validates an invite string. Whitespace around it is ignored. Nothing is logged: \a Parsed::error never contains the input.
Parsed<Invite> parseInvite(const QString& text);

/// The first invite found in free text (a chat message, a quoted mail): \a text may hold words, quotes or punctuation around it, and the
/// invite itself may be broken over several lines. Returns the clean `picklepack:` string, or an empty string when no part of \a text parses (or \a text is over 64 KiB).
/// It only selects text: parseInvite still validates every rule, so this never accepts anything the parser would refuse.
QString extractInvite(const QString& text);

/// The string to give to members. Pure inverse of parseInvite.
QString inviteToString(const Invite& invite);

/// 22 to 128 characters of A-Z a-z 0-9 - _ (at least 128 bits when generated from 16 random bytes).
bool isValidToken(const QString& token);

}  // namespace SharedPack

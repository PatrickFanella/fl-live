#pragma once
#include "project.hpp"
#include "fl_live/document_render.hpp"

struct AuthoredProject {
    ProjectSettings settings;
    fl_live::DocumentSnapshot identities;
};
fl_live::DocumentSnapshot defaultTemplateIdentities();
void validateTemplateIdentities(const fl_live::DocumentSnapshot& identities);
std::vector<fl_live::DocumentClipSource> templateSources(const ProjectSettings&, const fl_live::DocumentSnapshot&);
std::vector<fl_live::DocumentTrackGain> templateGains(const ProjectSettings&, const fl_live::DocumentSnapshot&);
QByteArray encodeAuthoredProject(const AuthoredProject&);
AuthoredProject decodeAuthoredProject(const QByteArray&);
AuthoredProject readAuthoredProject(const QString& path);
void writeAuthoredProject(const QString& path, const AuthoredProject&);

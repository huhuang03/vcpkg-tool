#include <vcpkg/base/util.h>

#include <vcpkg/versiondeserializers.h>

using namespace vcpkg;
using namespace vcpkg::Versions;

namespace
{
    constexpr StringLiteral BASELINE = "baseline";
    constexpr StringLiteral VERSION_RELAXED = "version";
    constexpr StringLiteral VERSION_SEMVER = "version-semver";
    constexpr StringLiteral VERSION_STRING = "version-string";
    constexpr StringLiteral VERSION_DATE = "version-date";
    constexpr StringLiteral PORT_VERSION = "port-version";

    struct VersionDeserializer final : Json::IDeserializer<std::pair<std::string, Optional<int>>>
    {
        VersionDeserializer(StringLiteral type, bool allow_hash_portversion)
            : m_type(type), m_allow_hash_portversion(allow_hash_portversion)
        {
        }
        StringView type_name() const override { return m_type; }

        Optional<std::pair<std::string, Optional<int>>> visit_string(Json::Reader& r, StringView sv) override
        {
            auto it = std::find(sv.begin(), sv.end(), '#');
            StringView pv(it, sv.end());
            std::pair<std::string, Optional<int>> ret{std::string(sv.begin(), it), nullopt};
            if (m_allow_hash_portversion)
            {
                if (pv.size() == 1)
                {
                    r.add_generic_error(type_name(), "'#' in version text must be followed by a port version");
                }
                else if (pv.size() > 1)
                {
                    ret.second = Strings::strto<int>(pv.substr(1).to_string());
                    if (ret.second.value_or(-1) < 0)
                    {
                        r.add_generic_error(type_name(),
                                            "port versions after '#' in version text must be non-negative integers");
                    }
                }
            }
            else
            {
                if (pv.size() == 1)
                {
                    r.add_generic_error(type_name(), "invalid character '#' in version text");
                }
                else if (pv.size() > 1)
                {
                    r.add_generic_error(type_name(),
                                        "invalid character '#' in version text. Did you mean \"port-version\": ",
                                        pv.substr(1),
                                        "?");
                }
            }
            return std::move(ret);
        }
        StringLiteral m_type;
        bool m_allow_hash_portversion;
    };

    struct GenericVersionTDeserializer : Json::IDeserializer<VersionT>
    {
        GenericVersionTDeserializer(StringLiteral version_field) : m_version_field(version_field) { }
        StringView type_name() const override { return "a version object"; }

        Optional<VersionT> visit_object(Json::Reader& r, const Json::Object& obj) override
        {
            std::pair<std::string, Optional<int>> version;
            int port_version = 0;

            static VersionDeserializer version_deserializer{"version", false};

            r.required_object_field(type_name(), obj, m_version_field, version, version_deserializer);
            r.optional_object_field(obj, PORT_VERSION, port_version, Json::NaturalNumberDeserializer::instance);

            return VersionT{std::move(version.first), port_version};
        }
        StringLiteral m_version_field;
    };
}

namespace vcpkg
{
    static Optional<SchemedVersion> visit_optional_schemed_deserializer(StringView parent_type,
                                                                        Json::Reader& r,
                                                                        const Json::Object& obj,
                                                                        bool allow_hash_portversion)
    {
        Versions::Scheme version_scheme = Versions::Scheme::String;
        std::pair<std::string, Optional<int>> version;

        VersionDeserializer version_exact_deserializer{"an exact version string", allow_hash_portversion};
        VersionDeserializer version_relaxed_deserializer{"a relaxed version string", allow_hash_portversion};
        VersionDeserializer version_semver_deserializer{"a semantic version string", allow_hash_portversion};
        VersionDeserializer version_date_deserializer{"a date version string", allow_hash_portversion};

        bool has_exact = r.optional_object_field(obj, VERSION_STRING, version, version_exact_deserializer);
        bool has_relax = r.optional_object_field(obj, VERSION_RELAXED, version, version_relaxed_deserializer);
        bool has_semver = r.optional_object_field(obj, VERSION_SEMVER, version, version_semver_deserializer);
        bool has_date = r.optional_object_field(obj, VERSION_DATE, version, version_date_deserializer);
        int num_versions = (int)has_exact + (int)has_relax + (int)has_semver + (int)has_date;
        int port_version = version.second.value_or(0);
        bool has_port_version =
            r.optional_object_field(obj, PORT_VERSION, port_version, Json::NaturalNumberDeserializer::instance);

        if (has_port_version && version.second)
        {
            r.add_generic_error(parent_type, "\"port_version\" cannot be combined with an embedded '#' in the version");
        }

        if (num_versions == 0)
        {
            if (!has_port_version)
            {
                return nullopt;
            }
            else
            {
                r.add_generic_error(parent_type, "unexpected \"port_version\" without a versioning field");
            }
        }
        else if (num_versions > 1)
        {
            r.add_generic_error(parent_type, "expected only one versioning field");
        }
        else
        {
            if (has_exact)
            {
                version_scheme = Versions::Scheme::String;
            }
            else if (has_relax)
            {
                version_scheme = Versions::Scheme::Relaxed;
                auto v = Versions::RelaxedVersion::from_string(version.first);
                if (!v.has_value())
                {
                    r.add_generic_error(parent_type, "'version' text was not a relaxed version:\n", v.error());
                }
            }
            else if (has_semver)
            {
                version_scheme = Versions::Scheme::Semver;
                auto v = Versions::SemanticVersion::from_string(version.first);
                if (!v.has_value())
                {
                    r.add_generic_error(parent_type, "'version-semver' text was not a semantic version:\n", v.error());
                }
            }
            else if (has_date)
            {
                version_scheme = Versions::Scheme::Date;
                auto v = Versions::DateVersion::from_string(version.first);
                if (!v.has_value())
                {
                    r.add_generic_error(parent_type, "'version-date' text was not a date version:\n", v.error());
                }
            }
            else
            {
                Checks::unreachable(VCPKG_LINE_INFO);
            }
        }

        return SchemedVersion(version_scheme, VersionT{std::move(version.first), port_version});
    }

    SchemedVersion visit_required_schemed_deserializer(StringView parent_type,
                                                       Json::Reader& r,
                                                       const Json::Object& obj,
                                                       bool allow_hash_portversion)
    {
        auto maybe_schemed_version = visit_optional_schemed_deserializer(parent_type, r, obj, allow_hash_portversion);
        if (auto p = maybe_schemed_version.get())
        {
            return std::move(*p);
        }
        else
        {
            r.add_generic_error(parent_type, "expected a versioning field (example: ", VERSION_STRING, ")");
            return {};
        }
    }

    View<StringView> schemed_deserializer_fields()
    {
        static const StringView t[] = {VERSION_RELAXED, VERSION_SEMVER, VERSION_STRING, VERSION_DATE, PORT_VERSION};
        return t;
    }

    void serialize_schemed_version(Json::Object& out_obj,
                                   Versions::Scheme scheme,
                                   const std::string& version,
                                   int port_version,
                                   bool always_emit_port_version)
    {
        auto version_field = [](Versions::Scheme version_scheme) {
            switch (version_scheme)
            {
                case Versions::Scheme::String: return VERSION_STRING;
                case Versions::Scheme::Semver: return VERSION_SEMVER;
                case Versions::Scheme::Relaxed: return VERSION_RELAXED;
                case Versions::Scheme::Date: return VERSION_DATE;
                default: Checks::unreachable(VCPKG_LINE_INFO);
            }
        };

        out_obj.insert(version_field(scheme), Json::Value::string(version));

        if (port_version != 0 || always_emit_port_version)
        {
            out_obj.insert(PORT_VERSION, Json::Value::integer(port_version));
        }
    }

    Json::IDeserializer<VersionT>& get_versiont_deserializer_instance()
    {
        static GenericVersionTDeserializer deserializer(VERSION_STRING);
        return deserializer;
    }

    Json::IDeserializer<VersionT>& get_versiontag_deserializer_instance()
    {
        static GenericVersionTDeserializer deserializer(BASELINE);
        return deserializer;
    }
}

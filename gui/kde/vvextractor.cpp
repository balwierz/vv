// KFileMetaData::ExtractorPlugin: surfaces row/column counts, schema, codec,
// and generator for vv data files in Dolphin's Information Panel and the
// Baloo index. Thin wrapper over vv_probe_meta (which links libvvcore).
// Installed to $PLUGINDIR/kf6/kfilemetadata/.
#include <kfilemetadata/extractorplugin.h>
#include <kfilemetadata/extractionresult.h>
#include <kfilemetadata/properties.h>
#include <kfilemetadata/typeinfo.h>

#include <QString>

#include "metaprobe.h"

using namespace KFileMetaData;

class VvExtractor : public ExtractorPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID kfilemetadata_extractor_iid FILE "vvextractor.json")
    Q_INTERFACES(KFileMetaData::ExtractorPlugin)
public:
    explicit VvExtractor(QObject* parent = nullptr) : ExtractorPlugin(parent) {}

    QStringList mimetypes() const override {
        return {
            QStringLiteral("application/vnd.apache.parquet"),
            QStringLiteral("application/vnd.apache.arrow.file"),
            QStringLiteral("application/x-hdf5"),
            QStringLiteral("application/x-npz"),
            QStringLiteral("application/vnd.sqlite3"),
            QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
            QStringLiteral("application/vnd.oasis.opendocument.spreadsheet"),
            // Genomic types (vv-formats.xml). Not CRAM (decoding may fetch the
            // reference over the network) or FASTA (a first record can be a
            // whole chromosome); the thumbnailer skips the same types.
            QStringLiteral("application/x-bam"),
            QStringLiteral("text/x-sam"),
            QStringLiteral("text/x-paf"),
            QStringLiteral("application/x-compressed-paf"),
            QStringLiteral("text/x-mpileup"),
            QStringLiteral("application/x-compressed-mpileup"),
            QStringLiteral("text/x-vcf"),
            QStringLiteral("application/x-compressed-vcf"),
            QStringLiteral("application/x-bcf"),
            QStringLiteral("text/x-bed"),
            QStringLiteral("application/x-compressed-bed"),
            QStringLiteral("text/x-bedgraph"),
            QStringLiteral("application/x-compressed-bedgraph"),
            QStringLiteral("text/x-gff"),
            QStringLiteral("application/x-compressed-gff"),
            QStringLiteral("text/x-gtf"),
            QStringLiteral("application/x-compressed-gtf"),
            QStringLiteral("application/x-bigwig"),
            QStringLiteral("application/x-bigbed"),
            QStringLiteral("text/x-fastq"),
            QStringLiteral("application/x-compressed-fastq"),
            QStringLiteral("application/x-2bit"),
            QStringLiteral("text/x-matrix-market"),
            QStringLiteral("application/x-compressed-matrix-market"),
        };
    }

    void extract(ExtractionResult* result) override {
        VvMeta m = vv_probe_meta(result->inputUrl());
        if (!m.ok) return;
        result->addType(Type::Spreadsheet);
        if (m.rows >= 0)
            result->add(Property::LineCount, (qlonglong)m.rows);
        if (!m.generator.isEmpty())
            result->add(Property::Generator, m.generator);
        // Alignment / variant files have a fixed column layout; their header
        // summary (reference, assembly, samples, …) says more than the schema.
        QString desc = m.genomic;
        if (desc.isEmpty()) {
            desc = QString::number(m.cols) + QStringLiteral(" columns");
            if (!m.schema.isEmpty()) desc += QStringLiteral(" — ") + m.schema;
        }
        result->add(Property::Description, desc);
        if (!m.footer.isEmpty())
            result->add(Property::Comment, m.footer);
    }
};

#include "vvextractor.moc"

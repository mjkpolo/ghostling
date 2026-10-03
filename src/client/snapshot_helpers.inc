#ifdef GMUX_CLIENT
static const SnapshotHeader *snapshot_header(const RenderSnapshotWire *wire)
{
    if (wire->len < sizeof(SnapshotRecord) + sizeof(SnapshotHeader))
        return NULL;
    SnapshotRecord record;
    memcpy(&record, wire->data, sizeof(record));
    if (record.kind != SNAPSHOT_HEADER || record.size != sizeof(SnapshotHeader))
        return NULL;
    return (const SnapshotHeader *)(wire->data + sizeof(record));
}

#endif

"""add per-device S3 recovery synchronization

Revision ID: 00023
Revises: 00022
Create Date: 2026-08-05
"""

from alembic import op
import sqlalchemy as sa


revision = "00023"
down_revision = "00022"
branch_labels = None
depends_on = None


def upgrade() -> None:
    # A first production attempt of this revision reached its autocommit block
    # and failed while building the timestamp-only unique index.  Keep the
    # revision restart-safe so Alembic can finish from that partial state.
    op.execute("DROP INDEX IF EXISTS uq_measurements_device_timestamp")
    op.execute("DROP INDEX IF EXISTS uq_meteo_readings_device_timestamp")
    op.execute("ALTER TABLE meteo_readings DROP CONSTRAINT IF EXISTS uq_meteo_reading_device_time")

    if not _has_column("measurements", "recovery_hash"):
        op.add_column("measurements", sa.Column("recovery_hash", sa.String(length=64), nullable=True))
    if not _has_column("meteo_readings", "recovery_hash"):
        op.add_column("meteo_readings", sa.Column("recovery_hash", sa.String(length=64), nullable=True))
    op.alter_column("meteo_readings", "timestamp_ms", existing_type=sa.BigInteger(), nullable=True)

    if not _has_table("device_s3_sync_configs"):
        op.create_table(
            "device_s3_sync_configs",
            sa.Column("device_id", sa.String(length=64), nullable=False),
            sa.Column("enabled", sa.Boolean(), nullable=False, server_default=sa.text("false")),
            sa.Column("bucket", sa.String(length=255), nullable=False),
            sa.Column("interval_minutes", sa.Integer(), nullable=False, server_default="10"),
            sa.Column("radiometer_prefix", sa.String(length=512), nullable=False, server_default="radiometers/"),
            sa.Column("meteo_prefix", sa.String(length=512), nullable=False, server_default="meteo/"),
            sa.Column("max_files_per_prefix", sa.Integer(), nullable=False, server_default="10"),
            sa.Column("last_radiometer_key", sa.Text(), nullable=True),
            sa.Column("last_meteo_key", sa.Text(), nullable=True),
            sa.Column("next_run_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
            sa.Column("last_started_at", sa.DateTime(timezone=True), nullable=True),
            sa.Column("last_success_at", sa.DateTime(timezone=True), nullable=True),
            sa.Column("last_error", sa.Text(), nullable=True),
            sa.Column("processed_files", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("inserted_measurements", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("inserted_meteo_readings", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("lease_owner", sa.String(length=36), nullable=True),
            sa.Column("lease_until", sa.DateTime(timezone=True), nullable=True),
            sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
            sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
            sa.CheckConstraint(
                "interval_minutes >= 1 AND interval_minutes <= 10080",
                name="ck_device_s3_sync_interval",
            ),
            sa.CheckConstraint(
                "max_files_per_prefix >= 1 AND max_files_per_prefix <= 100",
                name="ck_device_s3_sync_batch",
            ),
            sa.ForeignKeyConstraint(["device_id"], ["devices.id"], ondelete="CASCADE"),
            sa.PrimaryKeyConstraint("device_id"),
        )
    if not _has_index("device_s3_sync_configs", "ix_device_s3_sync_due"):
        op.create_index("ix_device_s3_sync_due", "device_s3_sync_configs", ["enabled", "next_run_at"])

    if not _has_table("device_s3_sync_objects"):
        op.create_table(
            "device_s3_sync_objects",
            sa.Column("id", sa.String(length=36), nullable=False),
            sa.Column("device_id", sa.String(length=64), nullable=False),
            sa.Column("bucket", sa.String(length=255), nullable=False),
            sa.Column("object_key", sa.Text(), nullable=False),
            sa.Column("etag", sa.String(length=128), nullable=False),
            sa.Column("kind", sa.String(length=16), nullable=False),
            sa.Column("status", sa.String(length=16), nullable=False),
            sa.Column("row_count", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("inserted_count", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("invalid_count", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("attempt_count", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("last_error", sa.Text(), nullable=True),
            sa.Column("next_retry_at", sa.DateTime(timezone=True), nullable=True),
            sa.Column("last_modified", sa.DateTime(timezone=True), nullable=True),
            sa.Column("processed_at", sa.DateTime(timezone=True), nullable=True),
            sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
            sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.text("now()")),
            sa.CheckConstraint("kind IN ('radiometer', 'meteo')", name="ck_device_s3_sync_object_kind"),
            sa.CheckConstraint("status IN ('done', 'failed', 'ignored')", name="ck_device_s3_sync_object_status"),
            sa.ForeignKeyConstraint(["device_id"], ["devices.id"], ondelete="CASCADE"),
            sa.PrimaryKeyConstraint("id"),
            sa.UniqueConstraint("device_id", "bucket", "object_key", name="uq_device_s3_sync_object"),
        )
    if not _has_index("device_s3_sync_objects", "ix_device_s3_sync_objects_device_kind"):
        op.create_index(
            "ix_device_s3_sync_objects_device_kind",
            "device_s3_sync_objects",
            ["device_id", "kind", "status"],
        )

    if not _has_index("measurements", "uq_measurements_device_recovery_hash"):
        op.create_index(
            "uq_measurements_device_recovery_hash",
            "measurements",
            ["device_id", "recovery_hash"],
            unique=True,
        )
    if not _has_index("meteo_readings", "uq_meteo_readings_device_recovery_hash"):
        op.create_index(
            "uq_meteo_readings_device_recovery_hash",
            "meteo_readings",
            ["device_id", "recovery_hash"],
            unique=True,
        )


def downgrade() -> None:
    op.drop_index("uq_meteo_readings_device_recovery_hash", table_name="meteo_readings")
    op.drop_index("uq_measurements_device_recovery_hash", table_name="measurements")
    op.drop_column("meteo_readings", "recovery_hash")
    op.drop_column("measurements", "recovery_hash")
    op.drop_index("ix_device_s3_sync_objects_device_kind", table_name="device_s3_sync_objects")
    op.drop_table("device_s3_sync_objects")
    op.drop_index("ix_device_s3_sync_due", table_name="device_s3_sync_configs")
    op.drop_table("device_s3_sync_configs")
    op.execute(
        "UPDATE meteo_readings SET timestamp_ms = "
        "(EXTRACT(EPOCH FROM timestamp) * 1000)::bigint WHERE timestamp_ms IS NULL"
    )
    op.alter_column("meteo_readings", "timestamp_ms", existing_type=sa.BigInteger(), nullable=False)
    op.create_unique_constraint(
        "uq_meteo_reading_device_time",
        "meteo_readings",
        ["device_id", "timestamp_ms"],
    )


def _has_table(table_name: str) -> bool:
    return sa.inspect(op.get_bind()).has_table(table_name)


def _has_column(table_name: str, column_name: str) -> bool:
    return any(
        column["name"] == column_name
        for column in sa.inspect(op.get_bind()).get_columns(table_name)
    )


def _has_index(table_name: str, index_name: str) -> bool:
    return any(
        index["name"] == index_name
        for index in sa.inspect(op.get_bind()).get_indexes(table_name)
    )

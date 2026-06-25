"""add gnss data datasets

Revision ID: 00020
Revises: 00019
Create Date: 2026-06-26
"""

from alembic import op
import sqlalchemy as sa


revision = "00020"
down_revision = "00019"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("SET LOCAL lock_timeout = '5s'")

    op.create_table(
        "gnss_data",
        sa.Column("id", sa.String(length=36), nullable=False),
        sa.Column("device_id", sa.String(length=64), nullable=False),
        sa.Column("name", sa.String(length=128), nullable=False),
        sa.Column("description", sa.Text(), nullable=True),
        sa.Column("measurement_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("start_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("end_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("last_import_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id"),
        sa.UniqueConstraint("device_id", "name", name="uq_gnss_data_device_name"),
    )
    op.create_index("ix_gnss_data_device_id", "gnss_data", ["device_id"])

    op.create_table(
        "gnss_data_measurements",
        sa.Column("id", sa.String(length=36), nullable=False),
        sa.Column("gnss_data_id", sa.String(length=36), nullable=False),
        sa.Column("measured_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("pw_mm", sa.Float(), nullable=False),
        sa.Column("spw_mm", sa.Float(), nullable=True),
        sa.Column("temperature_c", sa.Float(), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.ForeignKeyConstraint(["gnss_data_id"], ["gnss_data.id"], ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id"),
        sa.UniqueConstraint("gnss_data_id", "measured_at", name="uq_gnss_data_measurement_time"),
    )
    op.create_index(
        "ix_gnss_data_measurements_dataset_time",
        "gnss_data_measurements",
        ["gnss_data_id", "measured_at"],
    )

    op.alter_column("gnss_data", "measurement_count", server_default=None)


def downgrade() -> None:
    op.drop_index("ix_gnss_data_measurements_dataset_time", table_name="gnss_data_measurements")
    op.drop_table("gnss_data_measurements")
    op.drop_index("ix_gnss_data_device_id", table_name="gnss_data")
    op.drop_table("gnss_data")

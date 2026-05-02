"""add radiometer calibrations

Revision ID: 00012_radiometer_calibrations
Revises: 00011_bigint_timestamp_ms
Create Date: 2026-05-02 19:20:00.000000
"""

from alembic import op
import sqlalchemy as sa


revision = "00012"
down_revision = "00011"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "radiometer_calibrations",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("device_id", sa.String(length=64), sa.ForeignKey("devices.id"), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.Column("t_black_body_1", sa.Float(), nullable=False),
        sa.Column("t_black_body_2", sa.Float(), nullable=False),
        sa.Column("adc1_1", sa.Float(), nullable=False),
        sa.Column("adc2_1", sa.Float(), nullable=False),
        sa.Column("adc3_1", sa.Float(), nullable=False),
        sa.Column("adc1_2", sa.Float(), nullable=False),
        sa.Column("adc2_2", sa.Float(), nullable=False),
        sa.Column("adc3_2", sa.Float(), nullable=False),
        sa.Column("t_adc1", sa.Float(), nullable=False),
        sa.Column("t_adc2", sa.Float(), nullable=False),
        sa.Column("t_adc3", sa.Float(), nullable=False),
        sa.Column("adc1_slope", sa.Float(), nullable=False),
        sa.Column("adc2_slope", sa.Float(), nullable=False),
        sa.Column("adc3_slope", sa.Float(), nullable=False),
        sa.Column("adc1_intercept", sa.Float(), nullable=False),
        sa.Column("adc2_intercept", sa.Float(), nullable=False),
        sa.Column("adc3_intercept", sa.Float(), nullable=False),
        sa.Column("comment", sa.Text(), nullable=True),
    )
    op.create_index("ix_radiometer_calibrations_device_id", "radiometer_calibrations", ["device_id"], unique=False)
    op.create_index("ix_radiometer_calibrations_created_at", "radiometer_calibrations", ["created_at"], unique=False)


def downgrade() -> None:
    op.drop_index("ix_radiometer_calibrations_created_at", table_name="radiometer_calibrations")
    op.drop_index("ix_radiometer_calibrations_device_id", table_name="radiometer_calibrations")
    op.drop_table("radiometer_calibrations")

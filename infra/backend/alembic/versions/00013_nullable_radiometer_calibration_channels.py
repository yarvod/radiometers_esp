"""allow partial radiometer calibrations

Revision ID: 00013
Revises: 00012
Create Date: 2026-05-07 00:00:00.000000
"""

from alembic import op
import sqlalchemy as sa


revision = "00013"
down_revision = "00012"
branch_labels = None
depends_on = None


CHANNEL_COLUMNS = (
    "adc1_1",
    "adc2_1",
    "adc3_1",
    "adc1_2",
    "adc2_2",
    "adc3_2",
    "adc1_slope",
    "adc2_slope",
    "adc3_slope",
    "adc1_intercept",
    "adc2_intercept",
    "adc3_intercept",
)


def upgrade() -> None:
    for column in CHANNEL_COLUMNS:
        op.alter_column(
            "radiometer_calibrations",
            column,
            existing_type=sa.Float(),
            nullable=True,
        )


def downgrade() -> None:
    for column in CHANNEL_COLUMNS:
        op.alter_column(
            "radiometer_calibrations",
            column,
            existing_type=sa.Float(),
            nullable=False,
        )

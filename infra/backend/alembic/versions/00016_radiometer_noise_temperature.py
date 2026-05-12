"""add radiometer noise temperature

Revision ID: 00016
Revises: 00015
Create Date: 2026-05-12
"""

from alembic import op
import sqlalchemy as sa


revision = "00016"
down_revision = "00015"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("radiometer_calibrations", sa.Column("adc1_noise_temp", sa.Float(), nullable=True))
    op.add_column("radiometer_calibrations", sa.Column("adc2_noise_temp", sa.Float(), nullable=True))
    op.add_column("radiometer_calibrations", sa.Column("adc3_noise_temp", sa.Float(), nullable=True))
    op.execute(
        sa.text(
            """
            UPDATE radiometer_calibrations
            SET
                adc1_noise_temp = CASE WHEN adc1_intercept IS NULL THEN NULL ELSE -adc1_intercept END,
                adc2_noise_temp = CASE WHEN adc2_intercept IS NULL THEN NULL ELSE -adc2_intercept END,
                adc3_noise_temp = CASE WHEN adc3_intercept IS NULL THEN NULL ELSE -adc3_intercept END
            """
        )
    )


def downgrade() -> None:
    op.drop_column("radiometer_calibrations", "adc3_noise_temp")
    op.drop_column("radiometer_calibrations", "adc2_noise_temp")
    op.drop_column("radiometer_calibrations", "adc1_noise_temp")

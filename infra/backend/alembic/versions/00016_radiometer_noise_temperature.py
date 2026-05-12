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
                adc1_noise_temp = CASE
                    WHEN t_black_body_1 >= t_black_body_2
                        AND adc1_1 IS NOT NULL
                        AND adc1_2 IS NOT NULL
                        AND abs(adc1_2) >= 1e-12
                        AND abs((adc1_1 / adc1_2) - 1.0) >= 1e-12
                        THEN (t_black_body_1 - t_black_body_2 * (adc1_1 / adc1_2)) / ((adc1_1 / adc1_2) - 1.0)
                    WHEN t_black_body_2 > t_black_body_1
                        AND adc1_1 IS NOT NULL
                        AND adc1_2 IS NOT NULL
                        AND abs(adc1_1) >= 1e-12
                        AND abs((adc1_2 / adc1_1) - 1.0) >= 1e-12
                        THEN (t_black_body_2 - t_black_body_1 * (adc1_2 / adc1_1)) / ((adc1_2 / adc1_1) - 1.0)
                    ELSE NULL
                END,
                adc2_noise_temp = CASE
                    WHEN t_black_body_1 >= t_black_body_2
                        AND adc2_1 IS NOT NULL
                        AND adc2_2 IS NOT NULL
                        AND abs(adc2_2) >= 1e-12
                        AND abs((adc2_1 / adc2_2) - 1.0) >= 1e-12
                        THEN (t_black_body_1 - t_black_body_2 * (adc2_1 / adc2_2)) / ((adc2_1 / adc2_2) - 1.0)
                    WHEN t_black_body_2 > t_black_body_1
                        AND adc2_1 IS NOT NULL
                        AND adc2_2 IS NOT NULL
                        AND abs(adc2_1) >= 1e-12
                        AND abs((adc2_2 / adc2_1) - 1.0) >= 1e-12
                        THEN (t_black_body_2 - t_black_body_1 * (adc2_2 / adc2_1)) / ((adc2_2 / adc2_1) - 1.0)
                    ELSE NULL
                END,
                adc3_noise_temp = CASE
                    WHEN t_black_body_1 >= t_black_body_2
                        AND adc3_1 IS NOT NULL
                        AND adc3_2 IS NOT NULL
                        AND abs(adc3_2) >= 1e-12
                        AND abs((adc3_1 / adc3_2) - 1.0) >= 1e-12
                        THEN (t_black_body_1 - t_black_body_2 * (adc3_1 / adc3_2)) / ((adc3_1 / adc3_2) - 1.0)
                    WHEN t_black_body_2 > t_black_body_1
                        AND adc3_1 IS NOT NULL
                        AND adc3_2 IS NOT NULL
                        AND abs(adc3_1) >= 1e-12
                        AND abs((adc3_2 / adc3_1) - 1.0) >= 1e-12
                        THEN (t_black_body_2 - t_black_body_1 * (adc3_2 / adc3_1)) / ((adc3_2 / adc3_1) - 1.0)
                    ELSE NULL
                END
            """
        )
    )


def downgrade() -> None:
    op.drop_column("radiometer_calibrations", "adc3_noise_temp")
    op.drop_column("radiometer_calibrations", "adc2_noise_temp")
    op.drop_column("radiometer_calibrations", "adc1_noise_temp")

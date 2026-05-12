"""add temperature label map

Revision ID: 00015
Revises: 00014
Create Date: 2026-05-12
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "00015"
down_revision = "00014"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column(
            "temp_label_map",
            postgresql.JSONB(astext_type=sa.Text()),
            nullable=False,
            server_default=sa.text("'{}'::jsonb"),
        ),
    )
    op.execute(
        sa.text(
            """
            UPDATE devices AS d
            SET temp_label_map = COALESCE(
                (
                    SELECT jsonb_object_agg(address, label)
                    FROM (
                        SELECT address, label
                        FROM jsonb_array_elements_text(COALESCE(d.temp_addresses, '[]'::jsonb))
                            WITH ORDINALITY AS a(address, ord)
                        JOIN jsonb_array_elements_text(COALESCE(d.temp_labels, '[]'::jsonb))
                            WITH ORDINALITY AS l(label, ord)
                            USING (ord)
                        WHERE address <> '' AND label <> ''
                    ) AS pairs
                ),
                '{}'::jsonb
            )
            """
        )
    )
    op.alter_column("devices", "temp_label_map", server_default=None)


def downgrade() -> None:
    op.drop_column("devices", "temp_label_map")

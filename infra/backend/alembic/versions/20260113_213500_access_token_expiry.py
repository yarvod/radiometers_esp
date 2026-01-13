"""add access token expiry

Revision ID: 20260113_213500_access_token_expiry
Revises: 20240307_120000_init
Create Date: 2026-01-13 21:35:00.000000
"""

from alembic import op
import sqlalchemy as sa

# revision identifiers, used by Alembic.
revision = "20260113_213500_access_token_expiry"
down_revision = "20240307_120000_init"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("access_tokens", sa.Column("expires_at", sa.DateTime(timezone=True), nullable=True))
    op.execute("UPDATE access_tokens SET expires_at = now() WHERE expires_at IS NULL")
    op.alter_column("access_tokens", "expires_at", nullable=False)
    op.create_index("ix_access_tokens_expires_at", "access_tokens", ["expires_at"])


def downgrade() -> None:
    op.drop_index("ix_access_tokens_expires_at", table_name="access_tokens")
    op.drop_column("access_tokens", "expires_at")

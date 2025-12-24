FROM node:20-alpine
WORKDIR /app

# Install deps early for caching
COPY package*.json ./
RUN npm install

# Copy the rest
COPY . .

EXPOSE 3000
CMD ["npm", "run", "dev", "--", "--host", "0.0.0.0", "--port", "3000"]

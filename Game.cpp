#include "Game.hpp"

#include "Connection.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>

#include <glm/gtx/norm.hpp>

void Player::Controls::send_controls_message(Connection *connection_) const {
	assert(connection_);
	auto &connection = *connection_;

	uint32_t size = 5;
	connection.send(Message::C2S_Controls);
	connection.send(uint8_t(size));
	connection.send(uint8_t(size >> 8));
	connection.send(uint8_t(size >> 16));

	auto send_button = [&](Button const &b) {
		if (b.downs & 0x80) {
			std::cerr << "Wow, you are really good at pressing buttons!" << std::endl;
		}
		connection.send(uint8_t( (b.pressed ? 0x80 : 0x00) | (b.downs & 0x7f) ) );
	};

	send_button(left);
	send_button(right);
	send_button(up);
	send_button(down);
	send_button(jump);
}

bool Player::Controls::recv_controls_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;

	auto &recv_buffer = connection.recv_buffer;

	//expecting [type, size_low0, size_mid8, size_high8]:
	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::C2S_Controls)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	if (size != 5) throw std::runtime_error("Controls message with size " + std::to_string(size) + " != 5!");
	
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	auto recv_button = [](uint8_t byte, Button *button) {
		button->pressed = (byte & 0x80);
		uint32_t d = uint32_t(button->downs) + uint32_t(byte & 0x7f);
		if (d > 255) {
			std::cerr << "got a whole lot of downs" << std::endl;
			d = 255;
		}
		button->downs = uint8_t(d);
	};

	recv_button(recv_buffer[4+0], &left);
	recv_button(recv_buffer[4+1], &right);
	recv_button(recv_buffer[4+2], &up);
	recv_button(recv_buffer[4+3], &down);
	recv_button(recv_buffer[4+4], &jump);

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}


//-----------------------------------------

Game::Game() : mt(0x15466666) {
	for (uint8_t i = 0; i < GridWidth; i++) {
		for (uint8_t j = 0; j < GridHeight; j++) {
			GridBlock *block = new GridBlock();
			block->position = glm::vec2(GridStartX + i * GridBlockWidth, GridStartY + j * GridBlockHeight);

			do {
				block->color.r = mt() / float(mt.max());
				block->color.g = mt() / float(mt.max());
				block->color.b = mt() / float(mt.max());
			} while (block->color == glm::vec3(0.0f));

			grid.push_back(block);
		}
	}

	Ball *ball = new Ball();
	ball->position = glm::vec2(0.0f, -0.5f);
	ball->velocity = glm::vec2(0.0f, 1.0f);

	do {
		ball->color.r = mt() / float(mt.max());
		ball->color.g = mt() / float(mt.max());
		ball->color.b = mt() / float(mt.max());
	} while (ball->color == glm::vec3(0.0f));

	balls.push_back(ball);

	ball = new Ball();
	ball->position = glm::vec2(0.0f, 0.5f);
	ball->velocity = glm::vec2(0.0f, -1.0f);

	do {
		ball->color.r = mt() / float(mt.max());
		ball->color.g = mt() / float(mt.max());
		ball->color.b = mt() / float(mt.max());
	} while (ball->color == glm::vec3(0.0f));

	balls.push_back(ball);
}

Player *Game::spawn_player() {
	if (player_count >= 2) return nullptr;

	players.emplace_back();
	Player &player = players.back();

	// random point in the middle area of the arena:
	player.position.x = (ArenaMax.x + ArenaMin.x) / 2;

	if (player_count == 0) {
		// We are player 1
		player.position.y = ArenaMin.y + 0.02f * (ArenaMax.y - ArenaMin.y);
	} else {
		// We are player 2
		player.position.y = ArenaMin.y + 0.98f * (ArenaMax.y - ArenaMin.y);
	}

	do {
		player.color.r = mt() / float(mt.max());
		player.color.g = mt() / float(mt.max());
		player.color.b = mt() / float(mt.max());
	} while (player.color == glm::vec3(0.0f));

	player.color = glm::normalize(player.color);
	player.name = "Player " + std::to_string(next_player_number++);
	player_count++;

	return &player;
}

void Game::remove_player(Player *player) {
	bool found = false;
	for (auto pi = players.begin(); pi != players.end(); ++pi) {
		if (&*pi == player) {
			players.erase(pi);
			found = true;
			break;
		}
	}
	assert(found);
	player_count--;

	if (player_count != 0) {
		// Move remaining player to be the first player
		players.begin()->position.y = ArenaMin.y + 0.02f * (ArenaMax.y - ArenaMin.y);
	}
}

void Game::update(float elapsed) {
	//position/velocity update:
	for (auto &p : players) {
		glm::vec2 dir = glm::vec2(0.0f, 0.0f);
		if (p.controls.left.pressed) dir.x -= 1.0f;
		if (p.controls.right.pressed) dir.x += 1.0f;

		if (dir == glm::vec2(0.0f)) {
			//no inputs: just drift to a stop
			float amt = 1.0f - std::pow(0.5f, elapsed / (PlayerAccelHalflife * 2.0f));
			p.velocity = glm::mix(p.velocity, glm::vec2(0.0f,0.0f), amt);
		} else {
			//inputs: tween velocity to target direction
			dir = glm::normalize(dir);

			float amt = 1.0f - std::pow(0.5f, elapsed / PlayerAccelHalflife);

			//accelerate along velocity (if not fast enough):
			float along = glm::dot(p.velocity, dir);
			if (along < PlayerSpeed) {
				along = glm::mix(along, PlayerSpeed, amt);
			}

			//damp perpendicular velocity:
			float perp = glm::dot(p.velocity, glm::vec2(-dir.y, dir.x));
			perp = glm::mix(perp, 0.0f, amt);

			p.velocity = dir * along + glm::vec2(-dir.y, dir.x) * perp;
		}
		p.position += p.velocity * elapsed;

		//reset 'downs' since controls have been handled:
		p.controls.left.downs = 0;
		p.controls.right.downs = 0;
		p.controls.up.downs = 0;
		p.controls.down.downs = 0;
		p.controls.jump.downs = 0;
	}

	//collision resolution:
	for (auto &p1 : players) {
		//player/arena collisions:
		if (p1.position.x < ArenaMin.x + PlayerWidth / 2) {
			p1.position.x = ArenaMin.x + PlayerWidth / 2;
			p1.velocity.x = std::abs(p1.velocity.x);
		}
		if (p1.position.x > ArenaMax.x - PlayerWidth / 2) {
			p1.position.x = ArenaMax.x - PlayerWidth / 2;
			p1.velocity.x =-std::abs(p1.velocity.x);
		}
	}

	// Only start game if we have two players
	if (player_count != 2) return;

	for (auto *ball : balls) {
		ball->position += ball->velocity * BallSpeed * elapsed;

		auto rand_float = [&](float lo, float hi) {
			// Referenced StackOverflow https://stackoverflow.com/questions/5289613/generate-random-float-between-two-floats
			float rand_val = ((float) rand()) / (float) RAND_MAX; // Generates random between 0 and 1
			float diff = hi - lo;
			return lo + diff * rand_val;
		};

		//collision resolution:
		//ball/player collisions:
		glm::vec2 ball_velocity = ball->velocity;
		for (auto &p1 : players) {
			glm::vec2 positionDist = p1.position - ball->position;
			float len2 = glm::length2(positionDist);
			if (len2 == 0.0f) continue;

			if (ball->position.y - BallHeight / 2  > p1.position.y + PlayerHeight / 2 || ball->position.y + BallHeight / 2 < p1.position.y - PlayerHeight / 2 ) {
				continue;
			}
			if (ball->position.x - BallWidth / 2  > p1.position.x + PlayerWidth / 2 || ball->position.x + BallWidth / 2 < p1.position.x - PlayerWidth / 2 ) {
				continue;
			}

			ball->velocity = -ball_velocity;

			if (ball->velocity.x < 0) {
				ball->velocity.x = -rand_float(0.0f, 2.0f);
			} else {
				ball->velocity.x = rand_float(0.0f, 2.0f);
			}
			ball->velocity = glm::normalize(ball->velocity);
		}

		//ball/block collisions
		ball_velocity = ball->velocity;
		std::vector<GridBlock*> to_remove = std::vector<GridBlock*>();
		for (auto &block : grid) {
			glm::vec2 positionDist = block->position - ball->position;
			float len2 = glm::length2(positionDist);
			if (len2 == 0.0f) continue;

			if (ball->position.y - BallHeight / 2  > block->position.y + GridBlockHeight / 2 || ball->position.y + BallHeight / 2 < block->position.y - GridBlockHeight / 2 ) {
				continue;
			}
			if (ball->position.x - BallWidth / 2  > block->position.x + GridBlockWidth / 2 || ball->position.x + BallWidth / 2 < block->position.x - GridBlockWidth / 2 ) {
				continue;
			}

			to_remove.push_back(block);
			score += 100;

			if (ball->position.y - BallHeight / 2 >= block->position.y - GridBlockHeight / 2 && ball->position.y + BallHeight / 2 <= block->position.y + GridBlockHeight / 2) {
				// Left/right collision
				ball->velocity = ball_velocity;
				ball->velocity.x = -ball->velocity.x;
			} else {
				// Top/bottom collision
				ball->velocity = ball_velocity;
				ball->velocity.y = -ball->velocity.y;
			}
		}

		for (auto &block : to_remove) {
			if (rand_float(0.0f, 1.0f) < BallSpawnChance) {
				Ball *newBall = new Ball();
				newBall->position = block->position;
				newBall->velocity = glm::vec2(0.0f, 1.0f);
				newBall->color = block->color;
				balls.push_back(newBall);
			}

			auto index = std::find(begin(grid), end(grid), block);
			grid.erase(index);
		}

		//ball/arena collisions:
		if (ball->position.y < ArenaMin.y + BallHeight / 2 || ball->position.y > ArenaMax.y - BallHeight / 2) {
			auto index = std::find(begin(balls), end(balls), ball);
			balls.erase(index);
			score -= 50;
		}

		if (ball->position.x < ArenaMin.x + BallWidth / 2) {
			ball->position.x = ArenaMin.x + BallWidth / 2;
			ball->velocity.x = std::abs(ball->velocity.x);
		}
		if (ball->position.x > ArenaMax.x - BallWidth / 2) {
			ball->position.x = ArenaMax.x - BallWidth / 2;
			ball->velocity.x =-std::abs(ball->velocity.x);
		}
	}
}


void Game::send_state_message(Connection *connection_, Player *connection_player) const {
	assert(connection_);
	auto &connection = *connection_;

	connection.send(Message::S2C_State);
	//will patch message size in later, for now placeholder bytes:
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	size_t mark = connection.send_buffer.size(); //keep track of this position in the buffer

	//send player info helper:
	auto send_player = [&](Player const &player) {
		connection.send(player.position);
		connection.send(player.velocity);
		connection.send(player.color);
	
		//NOTE: can't just 'send(name)' because player.name is not plain-old-data type.
		//effectively: truncates player name to 255 chars
		uint8_t len = uint8_t(std::min< size_t >(255, player.name.size()));
		connection.send(len);
		connection.send_buffer.insert(connection.send_buffer.end(), player.name.begin(), player.name.begin() + len);
	};

	//send grid block info helper:
	auto send_block = [&](GridBlock const *block) {
		connection.send(block->position);
		connection.send(block->color);
	};

	//send ball info helper:
	auto send_ball = [&](Ball const *givenBall) {
		connection.send(givenBall->position);
		connection.send(givenBall->velocity);
		connection.send(givenBall->color);
	};

	//player count:
	connection.send(player_count);
	if (connection_player) send_player(*connection_player);
	for (auto const &player : players) {
		if (&player == connection_player) continue;
		send_player(player);
	}

	connection.send(grid.size());
	for (auto const &block : grid) {
		send_block(block);
	}

	connection.send(balls.size());
	for (auto const &ball : balls) {
		send_ball(ball);
	}
	connection.send(score);

	//compute the message size and patch into the message header:
	uint32_t size = uint32_t(connection.send_buffer.size() - mark);
	connection.send_buffer[mark-3] = uint8_t(size);
	connection.send_buffer[mark-2] = uint8_t(size >> 8);
	connection.send_buffer[mark-1] = uint8_t(size >> 16);
}

bool Game::recv_state_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;
	auto &recv_buffer = connection.recv_buffer;

	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::S2C_State)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	uint32_t at = 0;
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	//copy bytes from buffer and advance position:
	auto read = [&](auto *val) {
		if (at + sizeof(*val) > size) {
			throw std::runtime_error("Ran out of bytes reading state message.");
		}
		std::memcpy(val, &recv_buffer[4 + at], sizeof(*val));
		at += sizeof(*val);
	};

	players.clear();
	uint8_t num_players;
	read(&num_players);
	for (uint8_t i = 0; i < num_players; ++i) {
		players.emplace_back();
		Player &player = players.back();
		read(&player.position);
		read(&player.velocity);
		read(&player.color);
		uint8_t name_len;
		read(&name_len);
		//n.b. would probably be more efficient to directly copy from recv_buffer, but I think this is clearer:
		player.name = "";
		for (uint8_t n = 0; n < name_len; ++n) {
			char c;
			read(&c);
			player.name += c;
		}
	}

	grid.clear();
	size_t num_blocks;
	read(&num_blocks);
	for (size_t i = 0; i < num_blocks; ++i) {
		GridBlock *block = new GridBlock();
		read(&block->position);
		read(&block->color);
		grid.push_back(block);
	}

	balls.clear();
	size_t num_balls;
	read(&num_balls);
	for (size_t i = 0; i < num_balls; ++i) {
		Ball *ball = new Ball();
		read(&ball->position);
		read(&ball->velocity);
		read(&ball->color);
		balls.push_back(ball);
	}

	read(&score);

	if (at != size) throw std::runtime_error("Trailing data in state message.");

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}

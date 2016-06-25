#include <math.h>
#include <errno.h>

#include "phase1.h"
#include "bcrandom.h"
#include "obliv_common.h"

// computes inner product in Z_{2^64}
static uint64_t inner_prod_64(uint64_t *x, uint64_t *y, size_t n, size_t stride_x, size_t stride_y) {
	uint64_t xy = 0;
	for(size_t i = 0; i < n; i++) {
		xy += x[i*stride_x] * y[i*stride_y];
	}
	return xy;
}


// returns the party who owns a certain row
// the target vector with the hightes index is owned by the last party
static int get_owner(int row, config *conf) {
	check(row <= conf->d, "Invalid row %d", row);
	int party = 0;
	for(;party + 1 < conf->num_parties && conf->index_owned[party+1] <= row; party++);
	return party;

error:
	return -1;
}

// receives a message from peer, storing it in pmsg
// the result should be freed by the caller after use
static int recv_pmsg(SecureMultiplication__Msg **pmsg, ProtocolDesc *pd) {
	char *buf = NULL;
	check(pmsg && pd, "recv_pmsg: Arguments may not be null");
	*pmsg = NULL;
	size_t msg_size = 0;
	check(orecv(pd, 0, &msg_size, sizeof(msg_size)) == sizeof(msg_size), "orecv: %s", strerror(errno));
	buf = malloc(msg_size);
	check(buf, "Out of memory");
	check(orecv(pd, 0, buf, msg_size) == msg_size, "orecv: %s", strerror(errno));
	*pmsg = secure_multiplication__msg__unpack(NULL, msg_size, buf);
	check(*pmsg && (*pmsg)->vector, "msg__unpack: %s", strerror(errno));

	free(buf);
	return 0;
error:
	free(buf);
	return 1;
}

// Use protobuf-c's buffers to avoid copying data
typedef struct {
	ProtobufCBuffer base;
	ProtocolDesc *pd;
	int status;
} ProtocolDescBuffer;

static void protocol_desc_buffer_append(ProtobufCBuffer *buffer, size_t len, const uint8_t *data) {
	ProtocolDescBuffer *send_buffer = (ProtocolDescBuffer *) buffer;
	send_buffer->status = osend(send_buffer->pd, 0, data, len);
}

// sends the message pointed to by pmsg to peer
static int send_pmsg(SecureMultiplication__Msg *pmsg, ProtocolDesc *pd) {
	check(pmsg && pd, "send_pmsg: Arguments may not be null");
	size_t msg_size = secure_multiplication__msg__get_packed_size(pmsg);
	check(osend(pd, 0, &msg_size, sizeof(msg_size)) >= 0, "osend: %s", strerror(errno));
	ProtocolDescBuffer send_buffer = {.pd = pd};
	send_buffer.base.append = protocol_desc_buffer_append;

	secure_multiplication__msg__pack_to_buffer(pmsg, &send_buffer.base);
	check(send_buffer.status >= 0, "msg__pack_to_buffer: %s", strerror(errno));
	orecv(pd,0,NULL,0);
	return 0;
error:
	return 1;
}


int run_trusted_initializer(node *self, config *c, int precision) {
	BCipherRandomGen *gen = newBCipherRandomGen();
	int status;
	uint64_t *x = calloc(c->n , sizeof(uint64_t));
	uint64_t *y = calloc(c->n , sizeof(uint64_t));
	check(x && y, "malloc: %s", strerror(errno));
	SecureMultiplication__Msg pmsg_a, pmsg_b;
	secure_multiplication__msg__init(&pmsg_a);
	secure_multiplication__msg__init(&pmsg_b);
	pmsg_a.n_vector = c->n;
	pmsg_b.n_vector = c->n;
	pmsg_a.vector = y;
	pmsg_b.vector = x;
	for(size_t i = 0; i <= c->d; i++) {
		for(size_t j = 0; j <= i && j < c->d; j++) {
			// get parties a and b
			int party_a = get_owner(i, c);
			int party_b = get_owner(j, c);
			check(party_a >= 2, "Invalid owner %d for row %zd", party_a, i);
			check(party_b >= 2, "Invalid owner %d for row %zd", party_b, j);
			// if parties are identical, skip; will be computed locally
			if(party_a == party_b) {
				continue;
			}

			// generate random vectors x, y and value r
			uint64_t r = 0;
			randomizeBuffer(gen, (char *)x, c->n * sizeof(uint64_t));
			randomizeBuffer(gen, (char *)y, c->n * sizeof(uint64_t));
			randomizeBuffer(gen, (char *)&r, sizeof(uint64_t));
			uint64_t xy = inner_prod_64(x, y, c->n, 1, 1);

			// create protobuf message
			pmsg_a.value = xy-r;
			pmsg_b.value = r;
			//assert(pmsg_b.value == 0);

			status = send_pmsg(&pmsg_a, self->peer[party_a]);
			check(!status, "Could not send message to party A (%d)", party_a);
			status = send_pmsg(&pmsg_b, self->peer[party_b]);
			check(!status, "Could not send message to party B (%d)", party_b);
		}
	}

	// Receive and combine shares from peers for testing; TODO: remove
	uint64_t *share_A = NULL, *share_b = NULL;
	size_t d = c->d;
	share_A = calloc(d * (d + 1) / 2, sizeof(uint64_t));
	share_b = calloc(d, sizeof(uint64_t));

	SecureMultiplication__Msg *pmsg_in;
	for(int p = 2; p < c->num_parties; p++) {
		status = recv_pmsg(&pmsg_in, self->peer[p]);
		check(!status, "Could not receive result share_A from peer %d", p);
		for(size_t i = 0; i < d * (d + 1) / 2; i++) {
			share_A[i] += pmsg_in->vector[i];
		}
		secure_multiplication__msg__free_unpacked(pmsg_in, NULL);
		status = recv_pmsg(&pmsg_in, self->peer[p]);
		check(!status, "Could not receive result share_b from peer %d", p);
		for(size_t i = 0; i < d; i++) {
			share_b[i] += pmsg_in->vector[i];
		}
		secure_multiplication__msg__free_unpacked(pmsg_in, NULL);

	}
	for(size_t i = 0; i < c->d; i++) {
		for(size_t j = 0; j <= i; j++) {
			printf("%f ", fixed_to_double((fixed64_t) share_A[idx(i, j)], precision));
		}
		printf("\n");
	}

	free(x);
	free(y);
	releaseBCipherRandomGen(gen);
	return 0;

error:
	free(x);
	free(y);
	releaseBCipherRandomGen(gen);
	return 1;
}


int run_party(node *self, config *c, int precision, struct timespec *wait_total, uint64_t **res_A, uint64_t **res_b) {
	matrix_t data; // TODO: maybe use dedicated type for finite field matrices here
	vector_t target;
	data.value = target.value = NULL;
	int status;
	struct timespec wait_start, wait_end; // count how long we wait for other parties
	if(wait_total) {
		wait_total->tv_sec = wait_total->tv_nsec = 0;
	}
	SecureMultiplication__Msg *pmsg_ti = NULL,
				  *pmsg_in = NULL,
				  pmsg_out;
	secure_multiplication__msg__init(&pmsg_out);
	pmsg_out.n_vector = c->n;
	pmsg_out.vector = malloc(c->n * sizeof(uint64_t));
	uint64_t *share_A = NULL, *share_b = NULL;
	check(pmsg_out.vector, "malloc: %s", strerror(errno));

	// read inputs and allocate result buffer
	status = read_matrix(c->input, &data, precision);
	check(!status, "Could not read data");
	status = read_vector(c->input, &target, precision);
	check(!status, "Could not read target");
	size_t d = data.d[1];
	check(c->n == target.len && d == c->d && c->n == data.d[0],
		"Input dimensions invalid: (%zd, %zd), %zd",
		data.d[0], data.d[1], target.len);
	share_A = calloc(d * (d + 1) / 2, sizeof(uint64_t));
	share_b = calloc(d, sizeof(uint64_t));

	for(size_t i = 0; i < c->n; i++) {
		for(size_t j = 0; j < c->d; j++) {
			// rescale in advance
			data.value[i*c->d+j] = (fixed64_t) round(data.value[i*c->d+j] /
				(sqrt(pow(2,precision) * c->d * c->n)));
		}
		target.value[i] = (fixed64_t) round(target.value[i] /
			(sqrt(pow(2,precision) * c->d * c->n)));
	}

	for(size_t i = 0; i <= c->d; i++) {
		uint64_t *row_start_i, *row_start_j;
		size_t stride_i, stride_j;
		if(i < c->d) { // row of input matrix
			row_start_i = (uint64_t *) data.value + i;
			stride_i = d;
		} else { // row is target vector
			row_start_i = (uint64_t *) target.value;
			stride_i = 1;
		}
		for(size_t j = 0; j <= i && j < c->d; j++) {
			if(j < c->d) { // row of input matrix
				row_start_j = (uint64_t *) data.value + j;
				stride_j = d;
			} else { // row is target vector
				row_start_j = (uint64_t *) target.value;
				stride_j = 1;
			}
			int owner_i = get_owner(i, c);
			int owner_j = get_owner(j, c);
			check(owner_i >= 2, "Invalid owner %d for row %zd", owner_i, i);
			check(owner_j >= 2, "Invalid owner %d for row %zd", owner_j, j);

			uint64_t share;
			// if we own neither i or j, skip.
			if(owner_i != c->party-1 && owner_j != c->party-1) {
				continue;
			// if we own both, compute locally
			} else if(owner_i == c->party-1 && owner_i == owner_j) {
				share = inner_prod_64(row_start_i, row_start_j,
					c->n, stride_i, stride_j);
			} else {
				// receive random values from TI
				status = recv_pmsg(&pmsg_ti, self->peer[0]);
				check(!status, "Could not receive message from TI; %d %d", i, j);

				if(owner_i == c->party-1) { // if we own i but not j, we are party a
					int party_b = get_owner(j, c);
					check(party_b >= 2, "Invalid owner %d for row %zd", party_b, j);

					// receive (b', _) from party b
					clock_gettime(CLOCK_MONOTONIC, &wait_start);
					status = recv_pmsg(&pmsg_in, self->peer[party_b]);
					clock_gettime(CLOCK_MONOTONIC, &wait_end);
					if(wait_total) {
						wait_total->tv_sec += (wait_end.tv_sec - wait_start.tv_sec);
						wait_total->tv_nsec += (wait_end.tv_nsec - wait_start.tv_nsec);
					}
					check(!status, "Could not receive message from party B (%d)", party_b);

					// Send (a - y, _) to party b
					pmsg_out.value = 0;
					for(size_t k = 0; k < c->n; k++) {
						pmsg_out.vector[k] = row_start_i[k*stride_i] - pmsg_ti->vector[k];
					}
					status = send_pmsg(&pmsg_out, self->peer[party_b]);
					check(!status, "Could not send message to party B (%d)", party_b);

					// compute share as (b + x)y - (xy - r)
					share = inner_prod_64(pmsg_in->vector, pmsg_ti->vector, c->n, 1, 1);
					share -= pmsg_ti->value;

				} else { // if we own j but not i, we are party b
					int party_a = get_owner(i, c);
					check(party_a >= 2, "Invalid owner %d for row %zd", party_a, i);

					// send (b + y, _) to party a
					pmsg_out.value = 0;
					for(size_t k = 0; k < c->n; k++) {
						pmsg_out.vector[k] = row_start_j[k*stride_j] + pmsg_ti->vector[k];
						//assert(pmsg_out.vector[k] == 1023);
					}
					status = send_pmsg(&pmsg_out, self->peer[party_a]);
					check(!status, "Could not send message to party A (%d)", party_a);

					// receive (a', _) from party a
					clock_gettime(CLOCK_MONOTONIC, &wait_start);
					status = recv_pmsg(&pmsg_in, self->peer[party_a]);
					clock_gettime(CLOCK_MONOTONIC, &wait_end);
					if(wait_total) {
						wait_total->tv_sec += (wait_end.tv_sec - wait_start.tv_sec);
						wait_total->tv_nsec += (wait_end.tv_nsec - wait_start.tv_nsec);
					}
					check(!status, "Could not receive message from party A (%d)", party_a);

					// set our share to b(a - y) - r
					share = inner_prod_64(pmsg_in->vector, row_start_j, c->n, 1, stride_j);
					share -= pmsg_ti->value;

				}
				secure_multiplication__msg__free_unpacked(pmsg_in, NULL);
				secure_multiplication__msg__free_unpacked(pmsg_ti, NULL);
				pmsg_ti = pmsg_in = NULL;
			}
			// save our share
			if(i < c->d) {
				share_A[idx(i, j)] = share;
			} else {
				share_b[j] = share;
			}
		}
	}

	if(res_A){
		*res_A = share_A;
	} else {
		free(share_A);
	}
	if(res_b){
		*res_b = share_b;
	} else {
		free(share_b);
	}

	free(pmsg_out.vector);
	// send results to TI for testing; TODO: remove
	pmsg_out.vector = share_A;
	pmsg_out.n_vector = d * (d + 1) / 2;
	status = send_pmsg(&pmsg_out, self->peer[0]);
	check(!status, "Could not send share_A to TI");
	pmsg_out.vector = share_b;
	pmsg_out.n_vector = d;
	status = send_pmsg(&pmsg_out, self->peer[0]);
	check(!status, "Could not send share_b to TI");

	free(data.value);
	free(target.value);
	return 0;

error:
	free(data.value);
	free(target.value);
	free(pmsg_out.vector);
	if(res_A){
		*res_A = NULL;
	} else {
		free(share_A);
	}
	if(res_b){
		*res_b = NULL;
	} else {
		free(share_b);
	}
	if(pmsg_ti) secure_multiplication__msg__free_unpacked(pmsg_ti, NULL);
	if(pmsg_in) secure_multiplication__msg__free_unpacked(pmsg_in, NULL);
	return 1;
}

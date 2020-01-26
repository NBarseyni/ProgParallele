/*
	//gcc edge-detect.c bitmap.c -O2 -ftree-vectorize -fopt-info -mavx2 -fopt-info-vec-all apply-effect
	//UTILISER UNIQUEMENT DES BMP 24bits


	// ./apply-effect in out 3 SHARPEN
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "bitmap.h"
#include <stdint.h>
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>

#define DIM 3
#define LENGHT DIM
#define OFFSET DIM /2

const float KERNEL[DIM][DIM] = {{-1, -1,-1},
							   {-1,8,-1},
							   {-1,-1,-1}};

typedef struct Color_t {
	float Red;
	float Green;
	float Blue;
} Color_e;

typedef struct stack_t {
	Image* path;
	int count;
	int max;
	pthread_mutex_t lock;
	pthread_cond_t can_consume;
	pthread_cond_t can_produce;
} Stack;

typedef struct img_t {
	Image in;
	Image out;
	char* name;
} Img;

static char *in, *out;

void inline apply_convolution(Color_e* c, int a, int b, int x, int y, Image* img) __attribute__((always_inline));
void apply_convolution(Color_e* restrict c, int a, int b, int x, int y, Image* restrict img) {
    int xn = x + a - OFFSET;
    int yn = y + b - OFFSET;

    Pixel* p = &img->pixel_data[yn][xn];

    c->Red += ((float) p->r) * KERNEL[a][b];
    c->Green += ((float) p->g) * KERNEL[a][b];
    c->Blue += ((float) p->b) * KERNEL[a][b];
}

void apply_effect(Image* original, Image* new_i);
void apply_effect(Image* original, Image* new_i) {

	int w = original->bmp_header.width;
	int h = original->bmp_header.height;

	*new_i = new_image(w, h, original->bmp_header.bit_per_pixel, original->bmp_header.color_planes);

	for (int y = OFFSET; y < h - OFFSET; y++) {
		for (int x = OFFSET; x < w - OFFSET; x++) {
			Color_e c = { .Red = 0, .Green = 0, .Blue = 0};

			apply_convolution(&c, 0, 0, x, y, original);
            apply_convolution(&c, 0, 1, x, y, original);
            apply_convolution(&c, 0, 2, x, y, original);

            apply_convolution(&c, 1, 0, x, y, original);
            apply_convolution(&c, 1, 1, x, y, original);
            apply_convolution(&c, 1, 2, x, y, original);

            apply_convolution(&c, 2, 0, x, y, original);
            apply_convolution(&c, 2, 1, x, y, original);
            apply_convolution(&c, 2, 2, x, y, original);

			Pixel* dest = &new_i->pixel_data[y][x];
			dest->r = (uint8_t)  (c.Red <= 0 ? 0 : c.Red >= 255 ? 255 : c.Red);
			dest->g = (uint8_t) (c.Green <= 0 ? 0 : c.Green >= 255 ? 255 : c.Green);
			dest->b = (uint8_t) (c.Blue <= 0 ? 0 : c.Blue >= 255 ? 255 : c.Blue);
		}
	}
}

int getNumberFilesInDirectory(char* directory) {
	int count = 0;
	DIR* dir;
	struct dirent* entry;

	dir = opendir(directory);
	while ((entry = readdir(dir)) != NULL) {
		if (strstr(entry->d_name,".bmp")) {
			count++;	
		}		
	}
	return count;
}

void cleanDirectory(const char* directory) {
	DIR* dir = opendir(directory);
	struct dirent* nextFile;
	char filePath[256];

	while ((nextFile = readdir(dir)) != NULL) {
		sprintf(filePath, "%s%s", directory, nextFile->d_name);
		remove(filePath);
	}
	closedir(dir);
}

void set_directory(char *origin, char **target) {
    *target = malloc(strlen(origin));
    strcpy(*target, origin);
}

Stack *stack_init(int size) {
	Stack *stack = malloc(sizeof(Stack));
	pthread_cond_init(&stack->can_produce, NULL);
	pthread_cond_init(&stack->can_consume, NULL);
	pthread_mutex_init(&stack->lock, NULL);
	stack->path = malloc(sizeof(Image) * size);
	stack->max = size;
	stack->count = 0;
	return stack;
}

void push(Stack *stack, Image path) {
	stack->path[stack->count] = path;
	stack->count++;
}

Image pop(Stack *stack) {
	stack->count--;
	return stack->path[stack->count];
}

bool isEmpty(Stack *stack) {
	if (stack->count <= 0){
		return true;
	}
	return false;
}

Stack *fillStack(char* folder) {
	DIR* dir = opendir(folder);
	struct dirent* entry;
	int count = 0;

	char* path = NULL;
	Stack *imgs = stack_init(getNumberFilesInDirectory(folder));

	while ((entry = readdir(dir)) != NULL) {
		if (strstr(entry->d_name, ".bmp") != NULL) {
			path = malloc(strlen(folder) + strlen(entry->d_name));
			strcpy(path, folder);
			strcat(path, entry->d_name);
			printf("%s\n", path);
			Image newImg = open_bitmap(path);
			push(imgs, newImg);
		}
	}
	printf("return imgs\n");
	return imgs;
}

void* producer(void* arg) {
	Stack* stack = (Stack*) arg;
	while(true) {
		pthread_mutex_lock(&stack->lock);
		if (isEmpty(stack)) {
			pthread_cond_signal(&stack->can_consume);
			while (isEmpty(stack)) {
				if (isEmpty(stack)) {
					pthread_mutex_unlock(&stack->lock);
					return NULL;
				}
				pthread_cond_wait(&stack->can_produce, &stack->lock);
			}
			
		}
		Image img;
		img = pop(stack);
		pthread_mutex_unlock(&stack->lock);
		Image out_img;
		apply_effect(&img, &out_img);

		pthread_mutex_lock(&stack->lock);
		push(stack, out_img);
		pthread_mutex_unlock(&stack->lock);
	}
}

void* consumer(void* arg) {
	Stack* stack = (Stack*) arg;
	int size = stack->count;
	while(size > 0) {
		pthread_mutex_lock(&stack->lock);
		while (isEmpty(stack)) {
			pthread_cond_wait(&stack->can_consume, &stack->lock);
		}
		while (!isEmpty(stack)) {
			Image img = pop(stack);
			save_bitmap(img, out);
			size--;
		}
		pthread_cond_signal(&stack->can_produce);
		pthread_mutex_unlock(&stack->lock);
	}
	return NULL;
}

int main(int argc, char** argv) {
	// Regarder le nombre d'arguments (argv < 5)
	if (argc < 5) {
		printf("Arguments manquant :\nExemple : ./apply-effect \"./in/\" \"./out/\" 3 BOX_BLUR\n");
		exit(1);
	}

	// Regarder si le dossier in n'est pas vide (argv[1])
	int nbFiles = getNumberFilesInDirectory(argv[1]);
	if (nbFiles == 0) {
		printf("Aucun fichier bmp dans le dossier in\n");
		exit(1);
	}

	// Vider le fichier out (argv[2])
	set_directory(argv[2], &out);
	cleanDirectory(argv[2]);

	// Récupérer le nombre de thread demandé (argv[3])
	int nbThreads = atoi(argv[3]);

	// Ajouter les images à une stack
	Stack* imgs = fillStack(argv[1]);
	printf("%d\n", imgs->max);
	// Vérifier que le nombre de threads demandé n'est pas supérieur au nombre d'images
	if (nbThreads > imgs->max) {
		printf("On ne peut pas demander plus de threads qu'il n'y a d'images\n");
		exit(1);
	}

	// init pthread
	pthread_t threadsId[5];
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	printf("test1\n");
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	printf("test\n");

	// mask (args[4])

	// launch producteurs
	for (int i = 0; i < nbThreads; i++) {
		printf("%d\n", i);
		pthread_create(&threadsId[i], &attr, producer, (void*) imgs);
	}

	// launch consumer
	pthread_create(&threadsId[nbThreads - 1], NULL, consumer, (void*) imgs);
	// join consumer
	pthread_join(threadsId[nbThreads - 1], NULL);

	// Image img = open_bitmap("bmp_tank.bmp");
	// Image new_i;
	// apply_effect(&img, &new_i);
	// save_bitmap(new_i, "test_out.bmp");
	return 0;
}
#include<stdio.h>


void Display(int arr[]){

    printf("%d %d\n",sizeof(arr),sizeof(arr[0]));
}

int main(){
    int arr[5] = {1,2,3,4,5};
    Display(arr);
    return 0;
}